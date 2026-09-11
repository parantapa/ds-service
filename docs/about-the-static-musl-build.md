# About the static musl build

`scripts/Dockerfile` builds `ds-service` against musl on Alpine,
linked fully statically.
The final stage is `FROM scratch`,
so the resulting image holds nothing but the binary.

For the commands, see
[how to build the server](howto-build-the-server.md#static-musl-binary).

## Why the static build exists

A fully static binary has no loader dependencies.
So it runs on a host whose libc is older or different.
Also, you can drop it onto a cluster node or into an empty image
without carrying a userland with it.
That matters most where the deployment target is not the build machine.

## Why the Conan profile differs

Three details differ from the normal build,
and musl forces all three.

First, the profile marks `cmake` as platform-provided,
so the build uses Alpine's own cmake throughout.
Several of the dependencies tool-require cmake to build themselves.
The ConanCenter `cmake` package that otherwise satisfies them
repackages Kitware's glibc binaries,
which cannot run on musl.

Second, the Dockerfile passes `-static` as `CMAKE_EXE_LINKER_FLAGS`
on the final configure rather than through the profile.
Through the profile, `-static` also reaches
every dependency's configure-time link checks,
where the check does not test a static link.
The check then fails for reasons that have nothing to do with the build.
The final configure applies `-static` to `ds-service` alone.

Third, the same flags set `-Wl,-z,stack-size=1048576`.
musl takes the default stack size for a thread from `PT_GNU_STACK`,
and its built-in 128 KiB default is tight for gRPC's worker threads.

## What it costs

ConanCenter publishes no musl binaries,
so the first build compiles the whole dependency tree from source.
That cost is the price of the target, not a misconfiguration,
and the Conan cache absorbs it on subsequent builds.
