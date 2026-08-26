#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, json, math, re, struct, wave, zlib
from collections import Counter, defaultdict
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
import xml.etree.ElementTree as ET
import numpy as np

PPQ = 480
NATURAL_PC = {0, 2, 4, 5, 7, 9, 11}
STEP_PC = {'C':0,'D':2,'E':4,'F':5,'G':7,'A':9,'B':11}
HEADER = struct.Struct('<4sBBBBIIIIII')
TLV_HEADER = struct.Struct('<HBBI')
SEQ_HEADER = struct.Struct('<4sHHIIIHHI')
TEMPO = struct.Struct('<II')
TIME_SIG = struct.Struct('<IBBBB')
NOTE = struct.Struct('<IIBBBB')
TYPE_UTF8, TYPE_U32, TYPE_I32, TYPE_BYTES, TYPE_BOOL = 1,2,3,4,5
MAX_REPEAT_TIMES = 16
MAX_EXPANDED_MEASURES = 10000

@dataclass
class Event:
    start: Fraction
    duration: Fraction
    midi: int
    voice: str
    staff: str
    ties: frozenset[str]
    measure: str


def strip(tag): return tag.split('}',1)[-1]
def children(el,name): return [c for c in el if strip(c.tag)==name]
def child(el,name):
    for c in el:
        if strip(c.tag)==name: return c
    return None
def text(el,name,default=None):
    c=child(el,name)
    return c.text.strip() if c is not None and c.text else default

def midi_of(note):
    p=child(note,'pitch')
    if p is None: return None
    return 12*(int(text(p,'octave'))+1)+STEP_PC[text(p,'step')]+int(text(p,'alter','0'))

def qtick(q: Fraction): return int(round(float(q*PPQ)))

def _ending_numbers(value):
    numbers=set()
    for token in re.split(r'\s*,\s*', value.strip()):
        if not token: continue
        match=re.fullmatch(r'(\d+)(?:\s*-\s*(\d+))?',token)
        if match is None: raise ValueError(f'unsupported MusicXML ending number {value!r}')
        first=int(match.group(1)); last=int(match.group(2) or first)
        if first<1 or last<first or last>MAX_REPEAT_TIMES:
            raise ValueError(f'MusicXML ending number must be 1..{MAX_REPEAT_TIMES}: {value!r}')
        numbers.update(range(first,last+1))
    if not numbers: raise ValueError('MusicXML ending is missing its number')
    return frozenset(numbers)

def _measure_ending_sets(measures):
    result=[]; active=None
    for measure in measures:
        starts=[]; stops=[]
        for barline in children(measure,'barline'):
            for ending in children(barline,'ending'):
                kind=ending.attrib.get('type','').strip().lower()
                if kind=='start': starts.append(_ending_numbers(ending.attrib.get('number','')))
                elif kind in {'stop','discontinue'}: stops.append(kind)
                else: raise ValueError(f'unsupported MusicXML ending type {kind!r}')
        if len(starts)>1 or (starts and active is not None):
            raise ValueError('nested or overlapping MusicXML endings are not supported')
        if starts: active=starts[0]
        result.append(active)
        if stops:
            if len(stops)>1 or active is None:
                raise ValueError('MusicXML ending stop has no matching start')
            active=None
    if active is not None: raise ValueError('MusicXML ending is not closed')
    return result

def expand_measure_repeats(part):
    """Expand non-nested sequential repeats into a deterministic play order."""
    measures=children(part,'measure')
    jump_tags={'segno','coda'}
    jump_attrs={'dacapo','dalsegno','tocoda','fine','segno','coda'}
    if any(strip(el.tag) in jump_tags for el in part.iter()):
        raise ValueError('D.C./D.S./Segno/Coda/Fine playback jumps are not supported')
    for sound in (el for el in part.iter() if strip(el.tag)=='sound'):
        if jump_attrs.intersection(key.lower() for key in sound.attrib):
            raise ValueError('D.C./D.S./Segno/Coda/Fine playback jumps are not supported')

    ending_sets=_measure_ending_sets(measures)
    blocks=[]; open_start=None; segment_start=0
    for index,measure in enumerate(measures):
        forwards=[]; backwards=[]
        for repeat in (el for el in measure.iter() if strip(el.tag)=='repeat'):
            direction=repeat.attrib.get('direction','').strip().lower()
            if direction=='forward': forwards.append(repeat)
            elif direction=='backward': backwards.append(repeat)
            else: raise ValueError(f'unsupported MusicXML repeat direction {direction!r}')
        if len(forwards)>1 or len(backwards)>1:
            raise ValueError('multiple repeat marks on one measure are not supported')
        if forwards:
            if open_start is not None:
                raise ValueError('nested MusicXML repeats are not supported')
            open_start=index
        if backwards:
            start=segment_start if open_start is None else open_start
            if start>index: raise ValueError('MusicXML backward repeat precedes its forward repeat')
            raw_times=backwards[0].attrib.get('times','2').strip()
            if not raw_times.isdigit(): raise ValueError(f'invalid MusicXML repeat times {raw_times!r}')
            times=int(raw_times)
            if not 1<=times<=MAX_REPEAT_TIMES:
                raise ValueError(f'MusicXML repeat times must be 1..{MAX_REPEAT_TIMES}')
            blocks.append((start,index,times))
            open_start=None; segment_start=index+1
    if open_start is not None: raise ValueError('MusicXML forward repeat is not closed')
    if any(value is not None for value in ending_sets) and not blocks:
        raise ValueError('MusicXML ending has no associated repeat')
    for index,allowed in enumerate(ending_sets):
        if allowed is None: continue
        associated=next((block for block in reversed(blocks) if block[0]<=index),None)
        if associated is None or max(allowed)>associated[2]:
            raise ValueError('MusicXML ending number is not valid for its repeat count')

    expanded=[]; cursor=0
    for start,end,times in blocks:
        if start<cursor: raise ValueError('nested or overlapping MusicXML repeats are not supported')
        expanded.extend(measures[cursor:start])
        for play in range(1,times+1):
            for index in range(start,end+1):
                allowed=ending_sets[index]
                if allowed is None or play in allowed:
                    expanded.append(measures[index])
                    if len(expanded)>MAX_EXPANDED_MEASURES:
                        raise ValueError('expanded MusicXML exceeds the safe measure limit')
        cursor=end+1
    expanded.extend(measures[cursor:])
    if len(expanded)>MAX_EXPANDED_MEASURES:
        raise ValueError('expanded MusicXML exceeds the safe measure limit')
    return expanded, len(measures), len(blocks)

def parse(path: Path):
    root=ET.parse(path).getroot()
    if strip(root.tag)!='score-partwise': raise ValueError('v1 converter supports score-partwise MusicXML')
    title=text(child(root,'work'),'work-title',path.stem) if child(root,'work') is not None else path.stem
    creators=[]
    ident=child(root,'identification')
    if ident is not None:
        for c in ident:
            if strip(c.tag)=='creator' and c.text: creators.append(c.text.strip())
    raw=[]; tempos=[]; times=[]; keys=[]; unsupported=Counter(); total=Fraction(0)
    parts=children(root,'part')
    if len(parts) != 1:
        raise ValueError(f'sequence v1 currently requires exactly one part; found {len(parts)}')
    for feature in {'fermata', 'tuplet', 'time-modification', 'dynamics', 'pedal',
                    'octave-shift', 'wedge', 'articulations', 'ornaments', 'lyric'}:
        count=sum(1 for el in root.iter() if strip(el.tag)==feature)
        if count:
            unsupported[feature]=count
    repeat_source_measures=repeat_expanded_measures=repeat_blocks=0
    for part_idx,part in enumerate(parts):
        play_measures,repeat_source_measures,repeat_blocks=expand_measure_repeats(part)
        repeat_expanded_measures=len(play_measures)
        divisions=1; cursor=Fraction(0); last_start=Fraction(0)
        for meas in play_measures:
            attrs=child(meas,'attributes')
            if attrs is not None:
                d=text(attrs,'divisions')
                if d: divisions=int(d)
                k=child(attrs,'key')
                if k is not None: keys.append((cursor,int(text(k,'fifths','0')),text(k,'mode','')))
                t=child(attrs,'time')
                if t is not None: times.append((cursor,int(text(t,'beats','4')),int(text(t,'beat-type','4'))))
                if child(attrs,'transpose') is not None: unsupported['transpose']+=1
            for item in meas:
                tag=strip(item.tag)
                if tag=='direction':
                    found=[]
                    snd=child(item,'sound')
                    if snd is not None and 'tempo' in snd.attrib: found.append(float(snd.attrib['tempo']))
                    dt=child(item,'direction-type')
                    if dt is not None:
                        met=child(dt,'metronome')
                        if met is not None and text(met,'per-minute'): found.append(float(text(met,'per-minute')))
                    for bpm in found: tempos.append((cursor,bpm))
                elif tag=='backup': cursor-=Fraction(int(text(item,'duration','0')),divisions)
                elif tag=='forward': cursor+=Fraction(int(text(item,'duration','0')),divisions)
                elif tag=='note':
                    grace=child(item,'grace') is not None
                    dur=Fraction(int(text(item,'duration','0')),divisions)
                    chord=child(item,'chord') is not None
                    start=last_start if chord else cursor
                    if not chord: last_start=start
                    if child(item,'rest') is None:
                        m=midi_of(item)
                        if m is None: unsupported['unpitched_note']+=1
                        else:
                            ties=frozenset(c.attrib.get('type','') for c in item if strip(c.tag)=='tie')
                            raw.append(Event(start,dur,m,text(item,'voice','1'),text(item,'staff','1'),ties,meas.attrib.get('number','')))
                    if grace: unsupported['grace']+=1
                    elif not chord: cursor+=dur
                elif tag in {'harmony'}: unsupported[tag]+=1
            total=max(total,cursor)
    # merge ties per part-agnostic voice/staff/pitch (input currently one part)
    merged=[]; pending={}
    for e in sorted(raw,key=lambda x:(x.start,x.voice,x.staff,x.midi)):
        key=(e.voice,e.staff,e.midi)
        if 'stop' in e.ties and key in pending:
            base=pending[key]
            base.duration=max(base.duration,e.start+e.duration-base.start)
            if 'start' not in e.ties: pending.pop(key,None)
            continue
        merged.append(e)
        if 'start' in e.ties: pending[key]=e
    tempos=sorted(set(tempos),key=lambda x:x[0]) or [(Fraction(0),120.0)]
    # same-tick duplicate tempo: keep last
    td={qtick(t):b for t,b in tempos}; tempos=[(t,td[t]) for t in sorted(td)]
    ts={qtick(t):(n,d) for t,n,d in times}; times=[(t,*ts[t]) for t in sorted(ts)] or [(0,4,4)]
    return dict(root=root,title=title,creators=creators,events=merged,tempos=tempos,times=times,keys=keys,unsupported=unsupported,total=total,
                repeat_source_measures=repeat_source_measures,repeat_expanded_measures=repeat_expanded_measures,repeat_blocks=repeat_blocks)

def choose_octave_transpose(lo,hi,target_lo=48,target_hi=85):
    choices=list(range(-60,61,12))
    def cost(s):
        a,b=lo+s,hi+s
        overflow=max(0,target_lo-a)+max(0,b-target_hi)
        center=abs((a+b)-(target_lo+target_hi))/2
        return (overflow,center,abs(s))
    return min(choices,key=cost)

def tlv(tag,typ,value):
    if typ==TYPE_UTF8: data=str(value).encode()
    elif typ==TYPE_U32: data=struct.pack('<I',int(value))
    elif typ==TYPE_I32: data=struct.pack('<i',int(value))
    elif typ==TYPE_BYTES: data=bytes(value)
    elif typ==TYPE_BOOL: data=bytes([1 if value else 0])
    else: raise ValueError(typ)
    return TLV_HEADER.pack(tag,typ,0,len(data))+data

def tick_to_seconds(tick,tempos):
    sec=0.0; prev_tick=0; usq=500000
    for at,new_usq in tempos:
        if at>=tick: break
        sec+=(at-prev_tick)*usq/1_000_000/PPQ
        prev_tick=at; usq=new_usq
    sec+=(tick-prev_tick)*usq/1_000_000/PPQ
    return sec

def gain_for_midi(m):
    known_m=[48,50,52,53,55,57,59,60,62,64,65,67,69,71,72,74,76,77,79,81,83]
    known_db=[0,0,0,0,0,0,0,-1,-2,-3,-4,-5,-5,-6,-8,-11,-14,-14,-15,-17,-18]
    db=float(np.interp(m,known_m,known_db,left=known_db[0],right=known_db[-1]))
    return 10**(db/20)

def render_preview(events,tempos,transpose,path,sr=16000):
    end=max((tick_to_seconds(qtick(e.start+e.duration),tempos) for e in events),default=0)
    audio=np.zeros(int(math.ceil((end+0.1)*sr)),dtype=np.float32)
    for e in events:
        st=tick_to_seconds(qtick(e.start),tempos); en=tick_to_seconds(qtick(e.start+e.duration),tempos)
        a=max(0,int(round(st*sr))); b=min(len(audio),int(round(en*sr)))
        if b<=a: continue
        midi=e.midi+transpose; freq=440.0*2**((midi-69)/12)
        n=b-a; t=np.arange(n,dtype=np.float32)/sr
        env=np.ones(n,dtype=np.float32)
        attack=min(n,max(1,int(0.010*sr))); release=min(n,max(1,int(0.025*sr)))
        env[:attack]=np.linspace(0,1,attack,endpoint=True,dtype=np.float32)
        env[-release:]*=np.linspace(1,0,release,endpoint=True,dtype=np.float32)
        audio[a:b]+=np.sin(2*np.pi*freq*t,dtype=np.float32)*env*gain_for_midi(midi)
    peak=float(np.max(np.abs(audio))) if len(audio) else 0
    if peak>1: audio/=peak
    pcm=np.clip(audio*(18000/32767),-1,1)
    with wave.open(str(path),'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes((pcm*32767).astype('<i2').tobytes())

def build(input_path:Path,out:Path,profile_path:Path):
    parsed=parse(input_path); ev=parsed['events']
    pitches=[e.midi for e in ev]; lo,hi=min(pitches),max(pitches)
    transpose=choose_octave_transpose(lo,hi)
    voices={v:i for i,v in enumerate(sorted({e.voice for e in ev}))}
    note_rows=[]
    max_quant_error=0.0
    for e in ev:
        st=qtick(e.start); du=max(1,qtick(e.duration))
        max_quant_error=max(max_quant_error,abs(float(e.start*PPQ-st)),abs(float(e.duration*PPQ-du)))
        note_rows.append((st,du,e.midi,100,voices[e.voice],0))
    note_rows.sort(key=lambda x:(x[0],x[4],x[2]))
    tempo_rows=[(t,int(round(60_000_000/bpm))) for t,bpm in parsed['tempos']]
    time_rows=[]
    for tick,num,den in parsed['times']:
        if den<=0 or den&(den-1): raise ValueError(f'non-power-of-two denominator {den}')
        time_rows.append((tick,num,int(math.log2(den)),24,8))
    duration=max((st+du for st,du,*_ in note_rows),default=0)
    # active polyphony
    points=[]
    for st,du,*_ in note_rows: points.extend([(st,1),(st+du,-1)])
    active=peak_poly=0
    for _,d in sorted(points,key=lambda x:(x[0],x[1])): active+=d; peak_poly=max(peak_poly,active)
    teaching=all(m%12 in NATURAL_PC for m in pitches) and all(48<=m+transpose<=83 for m in pitches)
    payload=bytearray(SEQ_HEADER.pack(b'MSQ1',PPQ,NOTE.size,len(note_rows),len(tempo_rows),duration,len(time_rows),0,0))
    for row in tempo_rows: payload+=TEMPO.pack(*row)
    for row in time_rows: payload+=TIME_SIG.pack(*row)
    for row in note_rows: payload+=NOTE.pack(*row)
    src_hash=hashlib.sha256(input_path.read_bytes()).digest()
    profile=json.loads(profile_path.read_text())
    summary=f'Preserved {len(note_rows)} tied-merged notes; recommended transpose {transpose:+d} semitones; source pitch {lo}-{hi}; peak polyphony {peak_poly}.'
    creator='; '.join(parsed['creators'])
    meta=b''.join([
        tlv(1,TYPE_UTF8,parsed['title']),tlv(2,TYPE_UTF8,creator),tlv(3,TYPE_UTF8,'MusicXML'),
        tlv(4,TYPE_UTF8,profile['profile_id']),tlv(5,TYPE_BYTES,src_hash),tlv(16,TYPE_U32,PPQ),
        tlv(17,TYPE_U32,len(note_rows)),tlv(18,TYPE_U32,duration),tlv(19,TYPE_I32,transpose),
        tlv(20,TYPE_U32,lo),tlv(21,TYPE_U32,hi),tlv(22,TYPE_U32,peak_poly),
        tlv(23,TYPE_BOOL,teaching),tlv(24,TYPE_UTF8,summary),
        tlv(25,TYPE_UTF8,'PIANO_SYNTH')])
    header=HEADER.pack(b'MSPK',1,0,1,0,HEADER.size,len(meta),len(payload),zlib.crc32(meta)&0xffffffff,zlib.crc32(payload)&0xffffffff,1)
    out.mkdir(parents=True,exist_ok=True)
    package=out/(input_path.stem+'.mspkg'); package.write_bytes(header+meta+payload)
    render_preview(ev,tempo_rows,transpose,out/(input_path.stem+'-preview.wav'))
    duration_sec=tick_to_seconds(duration,tempo_rows)
    accidental=Counter(m%12 for m in pitches if m%12 not in NATURAL_PC)
    report=f'''---
title: {parsed['title']} — 设备兼容性报告
---

# {parsed['title']} — 设备兼容性报告

## 结论

这份 MusicXML 可以完整提取为单个乐谱 part 的时间序列，并保留其中的同时发声音符；但**不能由当前七个自然音键完整跟弹**。转换器保留原始十二平均律 MIDI 音高，没有把升降音静默替换为自然音。

- 输出包：`{package.name}`（MSPKG v1 / SEQUENCE）
- 试听：`{input_path.stem}-preview.wav`
- 原始音域：MIDI {lo}–{hi}
- 推荐播放移调：{transpose:+d} 半音（只作播放建议，包内事件保留原音高）
- 移调后音域：MIDI {lo+transpose}–{hi+transpose}
- 音符事件：{len(note_rows)}（连音线合并后）
- 峰值同时发音数：{peak_poly}
- 时长：{duration_sec:.2f} 秒
- PPQ：{PPQ}
- 最大量化误差：{max_quant_error:.6f} tick
- 反复展开：{parsed['repeat_blocks']} 段；{parsed['repeat_source_measures']} 个原始小节 → {parsed['repeat_expanded_measures']} 个播放小节

## 原谱结构

- MusicXML：{parsed['root'].attrib.get('version','unknown')} / score-partwise
- 速度：{', '.join(f'{60_000_000/us:.3g} BPM' for _,us in tempo_rows)}
- 拍号：{', '.join(f'{n}/{2**d}' for _,n,d,_,_ in time_rows)}
- 调号：{', '.join(f'{f:+d} fifths {mode}' for _,f,mode in parsed['keys']) or '未给出'}
- 原始音符中含升降音：{sum(1 for m in pitches if m%12 not in NATURAL_PC)} / {len(pitches)}
- 七自然音教学兼容：{'是' if teaching else '否'}
- 未实现/需人工确认的记谱特性：{dict(parsed['unsupported']) or '无阻断项'}

## 设备侧边界

当前 Play 固件只实测校准 C3–B5 的21个自然音。本包采用标准 MIDI 音高，为自动播放扩展十二平均律做准备。固件在实现 Song 播放器前不得直接把这些 MIDI 音高当作七键索引。

试听文件使用16kHz、16bit、单声道纯正弦，采用当前 `MASTER_PEAK=18000` 上限和21音衰减表的线性插值近似。它用于检查音高和节奏，不代表最终新增半音后的实板等响校准已经完成。

## 完整性

- 源 SHA-256：`{src_hash.hex()}`
- 包 SHA-256：`{hashlib.sha256(package.read_bytes()).hexdigest()}`
- metadata CRC-32：`{zlib.crc32(meta)&0xffffffff:08x}`
- payload CRC-32：`{zlib.crc32(payload)&0xffffffff:08x}`
'''
    (out/'compatibility-report.md').write_text(report)
    manifest={'title':parsed['title'],'source':input_path.name,'package':package.name,'preview':input_path.stem+'-preview.wav','event_count':len(note_rows),'duration_seconds':duration_sec,'original_midi_range':[lo,hi],'recommended_transpose':transpose,'transposed_range':[lo+transpose,hi+transpose],'peak_polyphony':peak_poly,'teaching_compatible':teaching,'repeat_blocks':parsed['repeat_blocks'],'source_measure_count':parsed['repeat_source_measures'],'expanded_measure_count':parsed['repeat_expanded_measures'],'source_sha256':src_hash.hex(),'package_sha256':hashlib.sha256(package.read_bytes()).hexdigest()}
    (out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
    print(json.dumps(manifest,ensure_ascii=False,indent=2))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('input',type=Path); ap.add_argument('-o','--output',type=Path,required=True); ap.add_argument('--profile',type=Path,default=Path(__file__).with_name('device-profile.json')); a=ap.parse_args(); build(a.input,a.output,a.profile)
if __name__=='__main__': main()
