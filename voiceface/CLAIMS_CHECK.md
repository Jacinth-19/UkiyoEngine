# Claims check: "Cyberpunk Voice–Face Game Pipeline (2026)"

Status of the claims in the user-pasted report. Nothing here has been verified against a primary source yet. The scaffold does not depend on any of these claims.

## Suspected errors (checked before anyone relies on them)

| Claim in report | Concern | Next step |
|---|---|---|
| Bark is a JAX model | Bark is a PyTorch model, as far as I know | Check the Bark repository and its requirements |
| Meta Voicebox is openly released | Voicebox was not openly released | Check Meta's research page and any licence |
| "FlutterVoice (Magenta)" | I cannot identify this project | Search for it by name in the Magenta organisation; if none, drop the claim |
| "Protoface" citation and reference `[54]` | Unverified | Locate the paper and check the citation number |
| `ubuntu-slim` runner | Unverified | Check the GitHub-hosted runner documentation |
| GitHub-hosted runner specs | Unverified | Check the current GitHub documentation |

## Unverified figures

| Claim | Next step |
|---|---|
| ElevenLabs latency about 75 ms | Check ElevenLabs' latency documentation and state the measurement conditions |
| "Resemble 2026 survey" | Locate the survey itself; the claim cannot be cited without it |
| Assimp exporter limits | Check the Assimp documentation and source for the exporter in question |

## Notes

- The scaffold uses none of the above. Voice cloning is a tone generator and the renderer is absent.
- Before any of these claims are used in a design document, each row needs a primary source or should be removed.
