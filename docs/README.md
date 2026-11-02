# RetroLink Documentation

- [Windows setup and build](../README.md)
- [DOS and Macintosh build environment setup](setup-environment.md)
- [Classic Macintosh build](macintosh-build.md)
- [MS-DOS setup, controls, and tests](../src/RetroLink.Dos/README.md)
- [Protocol and implementation plan](retrolink-plan.md)
- [Porting guide](PORTING_GUIDE.md)
- [Platform release notes](../RELEASE/README.md)

Windows protocol tests are in `tests/ClassicMacIde.Core.Tests`. Native DOS
protocol tests are in `src/RetroLink.Dos/tests`. The optional serial host test
is `scripts/windows/retrolink_host_test.py`; it requires Python and pyserial.