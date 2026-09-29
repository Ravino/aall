/* hexagon-probe.c -- trivial translation unit meson.build compiles with
 * `--target=hexagon-unknown-linux-musl` to check that the detected clang
 * actually has the Hexagon backend built in (some distro clang packages
 * are built with a reduced LLVM_TARGETS_TO_BUILD). Not linked into
 * anything, not part of hx_adaptive_loader itself. */
int m4_hexagon_probe(void)
{
	return 0;
}
