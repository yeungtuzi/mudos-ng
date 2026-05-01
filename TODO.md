# Have Time #
## SSH Support ##
- [ ] accept ssh connection at listening port
- [ ] make telnet port optional (default port is for SSH), with new config option "LegacyPort"

## network thread ##
- [ ] Pure network I/O thread

​	Process network connection, user command queue and output buffer in a dedicated thread.

## multi-core CPU ##
- [ ] eval object's hearbeat() with several threads, need to ensure the sync problem.

## packages ##
- [ ] For linux, deb or rpm or flatpak
- [ ] For Mac, homebrew

- [ ] Windows, TBD

