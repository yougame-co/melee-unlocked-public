Before committing the trial workflow, place these reviewed, non-secret files here:

- `yougame-online.patch`: the matching YouGame v2 source patch for v0.9.81. It must
  omit every diff for `GameSetup_gui.dat` and `slpCSS.dat`, including binary deletion
  hunks (G31). It must reverse-apply cleanly to the checked-out fork except for
  the three build files changed by this CI implementation (`port/CMakeLists.txt`,
  `.gitignore`, `.gitattributes`), which are excluded from that correspondence check.
- `yougame-native.json`: the canonical launch spec extracted from the old yg2
  package used by pc2's `pack-r2.py stage-extras`. Supply the actual spec; do not
  invent its schema. CI adds `--vanilla-only`, sets `net`, and adds/removes `direct`.

These two inputs were not supplied in `ci-ref` and are intentionally absent here.
The supervising session supplies them and their SHA-256 values before dispatch.
Alternative basenames are supported through the two path inputs, but files must
be directly inside this directory, with simple basenames and `.patch`/`.json`
extensions. Git attributes preserve their bytes on Windows.
