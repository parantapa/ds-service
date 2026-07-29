# Debian packaging for the ds-service server

The packaging in `debian/` builds a single binary package, `ds-service`,
that installs `/usr/bin/ds-service`.
The Python client is not packaged here.

## Requirements

- `build-essential`, `debhelper` (>= 13), `cmake` (>= 4.0), `g++` (>= 14), `git`
- [Conan](https://conan.io/) 2.x on `PATH` (`pip install conan`) —
    the C++ dependencies come from Conan.

## Building

    scripts/build-deb.sh

The `.deb`, `.buildinfo` and `.changes` files land in `build/deb/`. Options:

    scripts/build-deb.sh -v 1.0.3        # set the version explicitly
    scripts/build-deb.sh -o /tmp/debs    # different output directory

The script builds in a scratch copy of `git HEAD`, not in the working tree,
so commit before building and so the repo stays clean.

## Versioning

The package version is the one in the top entry of `debian/changelog`,
which is where `dpkg-buildpackage` reads it from as well.

`scripts/update-version.sh 1.0.4` sets that entry,
along with `project(VERSION ...)` in `CMakeLists.txt`
and the `VERSION` string in `cpp/ds-service.cpp`,
so run it before building to change the version.
It rewrites the top entry rather than adding one,
so add a new entry by hand (or with `dch`) when the change deserves one.

`build-deb.sh -v` overrides the version for a single build
by rewriting the changelog of the staged tree; the repo is not touched.

## Installing

    sudo apt install ./build/deb/ds-service_1.0.3_amd64.deb
    ds-service --address 0.0.0.0:5051

The dependencies are linked statically,
so the package only depends on the C and C++ runtimes;
`dh_shlibdeps` works those out at build time.
