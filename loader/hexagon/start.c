/*
 * Freestanding Hexagon process entry point.
 *
 * The kernel hands control to _start with r29 (sp) pointing at argc,
 * followed by argv[0..argc-1], NULL, envp[...], NULL, auxv. We only need
 * argc/argv, so hand the raw stack pointer to a small C helper.
 */

extern int hx_main(int argc, char **argv);
void hx_start_c(long *sp);

void hx_start_c(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	int rc = hx_main(argc, argv);
	__builtin_trap(); /* unreachable: hx_main() must call hx_exit_group() */
	(void)rc;
}

asm(
	".text\n"
	".global _start\n"
	".type _start, @function\n"
	"_start:\n"
	"  r0 = r29\n"
	"  call hx_start_c\n"
);
