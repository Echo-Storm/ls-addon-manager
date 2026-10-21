# Documentation

A map of everything written down in this repository. The [main README](../README.md) is the place to start; this page says where the rest is.

## Using it

| Read | For |
|---|---|
| [faq.md](faq.md) | Short answers to what people ask most |
| [../addons/DLSS5NR01/docs/user-guide.md](../addons/DLSS5NR01/docs/user-guide.md) | First run of Neural Rendering, step by step, and what to expect |
| [../addons/DLSS5NR01/docs/upscalers.md](../addons/DLSS5NR01/docs/upscalers.md) | The DLSS, FSR and XeSS Upscalers: every setting, what it costs, how to compare |
| [model-compatibility.md](model-compatibility.md) | Which `nvngx_dlssnr.dll` builds are known to run, and how each behaved |
| [antivirus.md](antivirus.md) | "My antivirus flags the download": why, and what to do |
| [capture-card-sources.md](capture-card-sources.md) | Consoles and capture cards: a 30 fps source seen at 60 or 120 Hz |
| [../CHANGELOG.md](../CHANGELOG.md), [../ROADMAP.md](../ROADMAP.md) | What changed in each release, and what is planned |

## Writing an addon

| Read | For |
|---|---|
| [addon-authors.md](addon-authors.md) | The exports, `addon.json`, the host interface, the shared look, icons |
| [api-compatibility.md](api-compatibility.md) | What the 1.x interface promises to keep stable |
| [../examples/SampleAddon](../examples/SampleAddon) | A small, tested addon to copy |

## How it is built

| Read | For |
|---|---|
| [../addons/DLSS5NR01/docs/architecture.md](../addons/DLSS5NR01/docs/architecture.md) | How a frame gets from Lossless Scaling to the model and back without it waiting |
| [../addons/DLSS5NR01/docs/building.md](../addons/DLSS5NR01/docs/building.md) | Prerequisites and build steps; [../tools/README.md](../tools/README.md) lists every script and test program |
| [../addons/DLSS5NR01/docs/dlssnr-knobs.md](../addons/DLSS5NR01/docs/dlssnr-knobs.md) | What the neural model actually listens to (measured) |
| [../addons/DLSS5NR01/docs/vsr.md](../addons/DLSS5NR01/docs/vsr.md) | The Video Super Resolution prototype (not in the package) |
| [../NOTICE.md](../NOTICE.md), [../DISCLAIMER.md](../DISCLAIMER.md), [../LICENSE](../LICENSE) | Third-party licences, what this project is not, the MIT licence |

## Research notes

Written down so they are not lost; none of it is in a product unless a release note says so.

| Read | For |
|---|---|
| [frame-generation-research.md](frame-generation-research.md) | Measuring frame generators and upscalers on recordings; the numbers behind the defaults |
| [own-generator-plan.md](own-generator-plan.md) | **EchoFG**, our own frame generator (test build only): what the lab says, the plan, fast motion, the scores |
| [mfg-super-motion-study.md](mfg-super-motion-study.md), [mfg-super-motion-engine-notes.md](mfg-super-motion-engine-notes.md) | Another project's public files, ideas only, and EchoFG's cost and quality against it |
| [optical-flow-study.md](optical-flow-study.md) | NVIDIA's optical flow hardware against our own estimator |
| [lossless-scaling-internals.md](lossless-scaling-internals.md) | What Lossless Scaling's own files show (its settings keys, its shaders, how its frame rate counter works and is hooked) |
| [lsfg-vk-study.md](lsfg-vk-study.md) | How LSFG 3.1 works under the hood, as far as an open reimplementation shows |
| [magpie-fork-study.md](magpie-fork-study.md), [magpie-dissection.md](magpie-dissection.md) | A read of the Magpie experimental fork, and where this project goes further |
| [repeat-skip-spec.md](repeat-skip-spec.md) | Skipping the work on a picture that is exactly the one before |
| [dual-gpu.md](dual-gpu.md) | Running the model on a second graphics card |
| [dlss-4.5-research.md](dlss-4.5-research.md) | What a DLSS 4.5 addon could do from Lossless Scaling's frames |

## For whoever continues the work

[../handoff/README.md](../handoff/README.md) says how to pick the project up on another machine; [../handoff/BACKLOG.md](../handoff/BACKLOG.md) is everything not done yet (with what has been read line by line); [../handoff/BOARD.md](../handoff/BOARD.md) is the board (`python tools/dev.py board`); the `handoff/release-*.md` files are the release notes to self.
