Optimization framework runtime logging: 28/02/2026
Optimization framework runtime logging: 14/03/2026
//-----------------------------------------------------------  

ansorosa@jetson:~/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53$ code .
ansorosa@jetson:~/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53$ cd build/
ansorosa@jetson:~/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build$ sudo ./MySystem
[sudo] password for ansorosa: 
[2026-03-14 16:12:37.436] [info] [SdlDisplayConcrete] Enabling jetson_clocks for sustained 60 FPS
[16:12:38.796] [info] Initializing Framework v19...
[16:12:38.801] [info] Thread pool started with 2 threads
[16:12:38.803] [info] [SharedQueue] Initialized with maxSize: 200.
[16:12:38.803] [info] [SharedQueue] Initialized with maxSize: 200.
[16:12:38.804] [info] [SharedQueue] Initialized with maxSize: 300.
[16:12:38.804] [info] [ConfigManager] Queues created: cam2Alg=200, cam2Disp=200, alg2Disp=300
[16:12:38.805] [info] [Aggregator] Initialized with config: retention=2000s, prune=5s, maxPending=2000, maxHistory=10000
[16:12:38.805] [info] [ConfigManager] Aggregator initialized: merge_wait_ms=50ms, retention=2000s
[16:12:38.805] [info] [LynsynModule] Validating configuration...
[16:12:38.805] [info] [LynsynModule] Ensured directory: /home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build/metrics_csv
[16:12:38.805] [info] [LynsynModule] Using outputCSV: '/home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build/metrics_csv/lynsyn_output.csv'
[16:12:38.806] [info] [ConfigManager] Lynsyn module initialized.
[16:12:38.806] [info] [CameraModule] Validating configuration...
[16:12:38.806] [info] [AlgorithmModule] Validating configuration...
[16:12:38.806] [info] [DisplayModule] Validating configuration...
[16:12:38.806] [info] [ConfigManager] Initializing Evolutionary Selector (ERL)...
[16:12:38.806] [info] [EvolutionarySelector] Validating configuration...
[16:12:38.807] [info] [EvolutionarySelector] Validation successful
[16:12:38.807] [info] [ConfigManager] Pipeline built: 7 modules.
[16:12:38.807] [info] [LynsynModule] Validating configuration...
[16:12:38.807] [info] [LynsynModule] Ensured directory: /home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build/metrics_csv
[16:12:38.808] [info] [LynsynModule] Using outputCSV: '/home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build/metrics_csv/lynsyn_output.csv'
[16:12:38.808] [info] [CameraModule] Validating configuration...
[16:12:38.808] [info] [AlgorithmModule] Validating configuration...
[16:12:38.812] [info] [DisplayModule] Validating configuration...
[16:12:38.812] [info] [EvolutionarySelector] Validating configuration...
[16:12:38.812] [info] [EvolutionarySelector] Validation successful
[16:12:38.812] [info] [ConfigManager] All modules validated.
[16:12:38.812] [info] [ConfigManager] Starting pipeline modules...
[16:12:38.812] [info] [ConfigManager] Starting Aggregator...
[16:12:38.812] [info] [ConfigManager] Starting Module [1/7]
[16:12:38.812] [info] [SoCModule] Starting...
[16:12:39.496] [info] [SoCConcrete] SoC monitoring started with command 'tegrastats --interval 500'.
[16:12:39.496] [info] [SoCModule] Started.
[16:12:39.547] [info] [ConfigManager] Starting Module [2/7]
[16:12:39.547] [info] [LynsynModule] Starting...
[16:12:39.547] [info] [LynsynMonitorConcrete] Initializing Lynsyn library...
[16:12:39.567] [info] [LynsynMonitorConcrete] Lynsyn initialized (attempt 1/5)
[16:12:39.569] [info] CSV logging enabled: /home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build/metrics_csv/lynsyn_output.csv
[16:12:39.569] [info] [Lynsyn] Library has no period_ms API; using legacy arming + host 1Hz scheduler.
[16:12:39.570] [info] [LynsynMonitorConcrete] Monitoring started
[16:12:39.570] [info] [Lynsyn] Monitoring started (target 100 ms / 10 Hz)
[16:12:39.570] [info] [LynsynModule] Started.
[16:12:39.605] [info] [Lynsyn] Library has no period_ms API; using legacy arming + host 1Hz scheduler.
[16:12:39.620] [info] [ConfigManager] Starting Module [3/7]
[16:12:39.621] [info] [CameraModule] Starting...
[16:12:39.621] [info] [DataConcrete] Attempting to open device at /dev/video0.
[16:12:39.737] [info] [DataConcrete] Device opened: /dev/video0 (fd=15)
[16:12:39.802] [info] Format locked: 320x240 YUYV
[16:12:39.856] [info] [DataConcrete] CONFIGURED: 320x240 YUYV @ 30 FPS, 8 buffers → READY TO STREAM
[16:12:39.879] [info] [DataConcrete] Streaming STARTED
[16:12:39.879] [info] [CameraModule] Streaming started.
[16:12:39.888] [info] [DataConcrete] Capture thread STARTED – PURE EVENT-DRIVEN (MAX FPS MODE)
[16:12:39.930] [info] [ConfigManager] Starting Module [4/7]
[16:12:39.930] [info] [AlgorithmModule] Starting...
[16:12:39.930] [info] [AlgorithmConcrete] Constructed with ZeroCopy queues
[16:12:39.931] [info] [AlgorithmConcrete] Started ? Invert
[16:12:39.932] [info] [AlgorithmModule] Started.
[16:12:39.958] [info] [AlgorithmConcrete] Thread started
[16:12:39.988] [info] [ConfigManager] Starting Module [5/7]
[16:12:39.988] [info] [DisplayModule] Starting...
[16:12:39.988] [info] [SdlDisplayConcrete] Constructed (SDL init deferred until initializeDisplay).
[16:12:41.508] [info] [DebugID] beginFrame frameId=0
[16:12:41.588] [info] [DebugID] beginFrame frameId=1
[16:12:41.588] [error] [DataConcrete] Camera Capture FPS DROPPED! Lower than 30 FPS.
[16:12:48.968] [info] [SdlDisplayConcrete] Initialized 320x240 (NV12 streaming textures).
[16:12:48.975] [info] [DisplayModule] Render loop started.
Segmentation fault
ansorosa@jetson:~/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_53/build$ 
