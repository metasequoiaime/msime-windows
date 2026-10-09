# Local CPU translation adapter

Optional Windows 10/11 helper for the existing `[custom_translation]` DeepLX
interface. Chinese and English sentences run locally using the two hash-pinned
Argos/OPUS models, CTranslate2 INT8 and SentencePiece. No GPU runtime, Torch,
Stanza, cloud account or inference-time network access is used.

This is a separate opt-in process; it is not bundled into the IME installer.
The native IME still checks its local dictionary first. A sentence is translated
when it is a candidate and the dictionary does not contain a translation.

## Installation and process control

Use ordinary user PowerShell and **64-bit Python 3.12**. The install directory
must be outside the IME's data directory, which an IME upgrade may replace.
Packaged terminal applications may virtualize `%LOCALAPPDATA%`; use an ordinary
PowerShell session or pass an explicit installation directory accessible at login.

```powershell
.\scripts\local_translation\install.ps1 -Python 'python.exe' -CpuPercent 1 -AutoStart
```

`-AutoStart` is optional. It writes the current user's `Run` entry; it does not
create a system service or require elevation. `-ImeConfig` overrides the path
found through the installed IME's `DataDir` registry value. `-Port` defaults to
1188. The installer creates an isolated venv, verifies/extracts models, waits for
health readiness, backs up the original IME TOML, enables candidate translation,
selects the loopback provider and requests `ConfigChanged` through the existing
auxiliary pipe. Other settings and cloud provider credentials are preserved.

```powershell
.\scripts\local_translation\control.ps1 -Action Status
.\scripts\local_translation\control.ps1 -Action Stop
.\scripts\local_translation\control.ps1 -Action Start
.\scripts\local_translation\control.ps1 -Action DisableAutoStart
```

Pass the same `-InstallDirectory` to both scripts when overriding its default.
Stop and disable startup before removing an installation. To disconnect it in
the IME, disable candidate translation or select another provider in Settings.
`backups/ime-config-before-*.toml` retains the exact previous configuration;
restoring it also restores settings made before installation. The backup and
`service.json` contain credentials and must not be attached to an issue or PR.

To change the CPU cap, stop the service, edit `cpu_percent` in `service.json`,
then start it. Values above 5%, zero, NaN and infinity are rejected. There is no
automatic escalation from the 1% default. Smaller caps and slower CPUs increase
latency and can cause the native client's timeout to be reached.

## Resource and HTTP contract

- A Windows Job Object uses `ENABLE | HARD_CAP` (`CpuRate = percent * 100`) and
  verifies the configured rate and process membership before importing the model
  runtime. Failure to establish the cap stops startup. All service threads are
  in this job. Priority is below normal; CTranslate2 intra/inter threads are 1.
- The percentage is **total system CPU capacity**, measured over Windows job
  scheduling intervals. It is not a limit on every sub-millisecond sample or on
  the IME, dependency installer, other applications, or memory consumption.
  Single short requests can produce noisy CPU readings. GPU is not used.
- The service binds **only `127.0.0.1`**, requires its generated Bearer token,
  rejects browser origins, logs no requests and retains up to 256 translations
  in memory. Cache expiry is checked on requests; there is no disk text cache.
- `POST /translate` accepts `{"text":"...","source_lang":"ZH","target_lang":"EN"}`
  and returns `{"code":200,"data":"...","cached":false}`. `EN` to `ZH` is also
  supported. `GET /health` and `POST /shutdown` require the same token.
- One inference runs at a time; other uncached requests return 429 instead of
  building a queue. At most four HTTP connections are handled concurrently.
  Request bodies, sentences, token lengths and decoder lengths are bounded.
- Decoding has a 2.2-second deadline, checked between generated tokens, before
  the IME client's 2.5-second timeout. Encoder/token processing cannot be
  interrupted mid-operation. Timeout/length-limited fragments are not returned
  as complete translations. Normal short sentences are the intended workload;
  paragraphs, model quality and latency on other machines are not guaranteed.

The cap API is documented by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information).
Nested job caps can further reduce available CPU; DFSS environments may reject
rate control, in which case startup fails. The model runtime is described in
the [CTranslate2 documentation](https://opennmt.net/CTranslate2/hardware_support.html).

## Verification

Unit tests need no ML packages or downloads:

```powershell
python -m unittest discover -s tests -p test_local_translation.py -v
```

The optional live benchmark uses synthetic sentences only, and keeps its report
outside the repository. It measures HTTP latency and normalizes process CPU time
by wall time and logical processors. Start with a fresh service to measure cold
cache misses. Install `psutil==7.2.2` into its venv to run it:

```powershell
$installDir = Join-Path $env:LOCALAPPDATA 'MetasequoiaLocalTranslation'
$runtimePython = Join-Path $installDir 'venv\Scripts\python.exe'
& $runtimePython -m pip install psutil==7.2.2
& $runtimePython scripts/local_translation/benchmark.py --config (Join-Path $installDir 'service.json') --output (Join-Path $installDir 'benchmark.json')
```

Downloaded model packages are 74,481,402 and 70,743,021 bytes; the lock pins both
hashes. Only inference files and model attribution are extracted; the unused
Stanza sentence-segmentation model is omitted. Model files and personal runtime
configuration must stay outside the checkout.

## Model attribution

Packages: [Argos model index](https://github.com/argosopentech/argospm-index/blob/main/index.json),
`translate-zh_en-1_9` and `translate-en_zh-1_9`. Their included READMEs attribute
the original OPUS models to **Jörg Tiedemann and Santhosh Thottingal**,
*OPUS-MT — Building open translation services for the World*, EAMT 2020,
and specify [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The packages and attribution are preserved unchanged; only unused files are
omitted from the runtime extraction. Model weights are not committed or bundled.
