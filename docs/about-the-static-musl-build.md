# About the static musl build

`scripts/Dockerfile` builds `ds-service` against musl on Alpine,
linked fully statically,
and its final stage is `FROM scratch`,
so the resulting image holds nothing but the binary.

For the commands, see
[how to build the server](howto-build-the-server.md#static-musl-binary).

## Why it exists

A fully static binary has no loader dependencies,
so it runs on a host whose libc is older or simply different,
and it can be dropped onto a cluster node or into an empty image
without carrying a userland with it.
That matters most where the deployment target is not the build machine.

## Why the Conan profile differs

Two details differ from the normal build,
and both are forced rather than chosen.

The profile marks `cmake` as platform-provided,
so Alpine's own cmake is used throughout.
Several of the dependencies tool-require cmake to build themselves,
and the ConanCenter `cmake` package that would otherwise satisfy them
repackages Kitware's glibc binaries,
which cannot run on musl.

And `-static` is passed as `CMAKE_EXE_LINKER_FLAGS` on the final configure
rather than through the profile.
Passing it through the profile
would apply it to every dependency's configure-time link checks as well,
where a static link is not what is being tested
and the check fails for reasons that have nothing to do with the build.
Scoping it to the final configure applies it to `ds-service` alone.

## What it costs

ConanCenter publishes no musl binaries,
so the first build compiles the whole dependency tree from source.
This is the price of the target, not a misconfiguration,
and the Conan cache absorbs it on subsequent builds.
