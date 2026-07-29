# Debian packaging for the ds-service server

The packaging in `debian/` builds a single binary package, `ds-service`,
that installs `/usr/bin/ds-service`.
The Python client is not packaged here.

## Requirements

- `build-essential`, `debhelper` (>= 13), `cmake` (>= 4.0), `g++` (>= 14), `git`
- [Conan](https://conan.io/) 2.x on `PATH` (`pip install conan`) —
    the C++ dependencies come from Conan.
- `setuptools_scm` (`pip install setuptools_scm`),
    which supplies the package version;
    only needed when the version is not passed with `-v`.

## Building

    scripts/build-deb.sh

The `.deb`, `.buildinfo` and `.changes` files land in `build/deb/`. Options:

    scripts/build-deb.sh -v 1.0.3        # set the version explicitly
    scripts/build-deb.sh -o /tmp/debs    # different output directory

The script builds in a scratch copy of `git HEAD`, not in the working tree,
so commit before building and so the repo stays clean.

## Versioning

`build-deb.sh` takes the version from `setuptools_scm`,
i.e. from the git tags,
and writes it into the changelog of the staged tree before building.
The version in the committed `debian/changelog` is therefore
only what a plain `dpkg-buildpackage` run in the repo root would use;
the script overwrites it.

## Installing

    sudo apt install ./build/deb/ds-service_1.0.3_amd64.deb
    ds-service --address 0.0.0.0:5051

The dependencies are linked statically,
so the package only depends on the C and C++ runtimes;
`dh_shlibdeps` works those out at build time.
