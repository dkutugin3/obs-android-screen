# libobs Windows import library source

On Windows the OBS API is split across two modules:

- `libobs.def` — the `obs_*` API exported by **obs.dll**;
- `obs64.def` — `blog`, `bmalloc`, `bfree`, `obs_data_*` etc. exported by
  **obs64.exe** (the process executable re-exports the base API).

Plugins import from both; the build turns each .def into an import library
with dlltool (MinGW) or `lib /def` (MSVC) — see CMakeLists.txt.

Regenerate against a given OBS release:

    # obs.dll (obs_* API; note the padded ordinals like "[ 807]"):
    x86_64-w64-mingw32-objdump -p OBS/Windows/bin/64bit/obs.dll \
      | sed -nE 's/^\t\[ ?[0-9]+\] \+base\[ ?[0-9]+\] +[0-9a-f]+ +([A-Za-z_][A-Za-z0-9_@.]*)$/\1/p' \
      | sort -u > names.txt
    # obs64.exe (blog/bmalloc/base, different table format):
    x86_64-w64-mingw32-objdump -p OBS/Windows/bin/64bit/obs64.exe \
      | awk '$1 ~ /^[0-9a-f]{8}$/ && NF==4 && $3 ~ /^[0-9a-f]+$/ {print $4}' \
      | sort -u > names64.txt
    { echo "LIBRARY obs64.exe"; echo "EXPORTS"; sed 's/^/  /' names.txt; } > libobs.def

The build turns it into `libobs.lib` with dlltool (MinGW) or `lib /def`
(MSVC) — see CMakeLists.txt. Generated from OBS Studio 32.2.2 (x64).
