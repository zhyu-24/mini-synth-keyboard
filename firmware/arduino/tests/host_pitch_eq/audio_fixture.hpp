// Runs the actual complete audioRenderTask with only hardware/RTOS I/O stubbed.
void initializeTables() {
  constexpr float frequencies[7] = {261.63f, 293.66f, 329.63f, 349.23f,
                                    392.00f, 440.00f, 493.88f};
  for (int note = 0; note < NOTE_COUNT; ++note) {
    basePhaseSteps[note] = static_cast<uint32_t>(frequencies[note] * PHASE_SCALE / SAMPLE_RATE_HZ);
    for (int octave = 0; octave < 3; ++octave)
      noteGainByOctave[octave][note] = powf(10.0f, NOTE_ATTENUATION_DB[octave][note] / 20.0f);
  }
  for (int i = 0; i < SINE_TABLE_SIZE; ++i)
    sineTable[i] = static_cast<int16_t>(sin(2.0 * 3.141592653589793 * i / SINE_TABLE_SIZE) * 32767);
}

std::vector<int16_t> runAudio(int timbre, bool song, bool eq,
                             std::function<void(int)> changes = {}, int blocks = 40) {
  initializeTables();
  std::fill(std::begin(envelopes), std::end(envelopes), 0.0f);
  std::fill(std::begin(phases), std::end(phases), 0);
  glideSamplesRemaining = 0;
  startupWrites = testBlock = 0;
  testBlocks = blocks;
  captured.clear(); capturedGains.clear(); capturedWeights.clear();
  sharedNoteMask = 0; sharedOctaveOffset = 0;
  sharedActiveApp = song ? AppId::Song : AppId::Play;
  sharedPlayTimbre = static_cast<SongTimbre>(timbre);
  sharedSongTimbre = static_cast<SongTimbre>(timbre);
  sharedGlobalVolumePercent = 100;
  sharedSongStatus = song ? SongStatus::Playing : SongStatus::Stopped;
  sharedSongRestartRequested = false;
  sharedSongIndex = 0;
  sharedSongLoadState = SongLoadState::Ready;
  setEq(eq);
  // Four overlapping notes, then four simultaneous onsets, with room for tails.
  static const SongEvent events[] = {{0, 900, 60}, {1, 900, 64}, {2, 900, 67}, {3, 900, 72},
                                    {1400, 600, 62}, {1400, 600, 65},
                                    {1400, 600, 69}, {1400, 600, 74}};
  testSong.events = events; testSong.eventCount = 8; testSong.durationSamples = 10000;
  blockSetup = [=](int block) {
    if (!song) {
      sharedNoteMask = block < 18 ? 0x7f : (block < 25 ? 0 : 0x15);
      sharedOctaveOffset = block < 6 ? 0 : (block < 12 ? 1 : (block < 25 ? -1 : 1));
    }
    if (changes) changes(block);
  };
  const auto accesses = nvsAccesses;
  audioRenderTask(nullptr);
  assert(nvsAccesses == accesses);  // No NVS access from the audio task.
  blockSetup = {};
  return captured;
}
