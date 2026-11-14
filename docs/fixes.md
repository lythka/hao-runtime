# Fixes shown on the test laptop

Fixes to how hao runs macOS's own programs and third-party apps that were
shown on the test laptop (a MacBookPro9,2 running macOS 27.2.1, 26B131, under
hao) after the host tests. Performance changes are listed in
[performance](performance.md); each change's scope and limits are in
[ISSUES](ISSUES.md).

| Fix | What the user saw | Laptop test | Commits |
|---|---|---|---|
| Nils in the results of native generic functions; NodeCreateSemanticNodeQuery's client context | Finder quit at the end of a file query and on sidebar clicks | 2026-11-14, image f112087 (26B131): Finder stays up through sidebar navigation, view switches, Quick Look, Get Info and 14 minutes of use | aee7feb, 9568ead |
| SIPathFilterCreate's evaluate callback returns its struct indirectly | Spotlight's indexer (mds) quit while Finder searched | 2026-11-14, image f112087 (26B131): mds and Finder stay up for 15 minutes of Finder searches and Spotlight queries | eef78f8 |
| Guest dlopen resolves @rpath, @loader_path and @executable_path from the calling image | Xcode quit 4 seconds after launch | 2026-11-14, image f112087 (26B131): Xcode starts and stays up for about 3.5 minutes, until a plug-in load fails | 79bc70e |
