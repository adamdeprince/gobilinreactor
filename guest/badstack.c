// No stack access after replacing sp. Every trap must use the stub's own
// alternate signal stack, including when this is the last guest in the run.
__asm__(".global _start\n_start:\n"
        "mov x9, #1\nmov sp, x9\n"
        "mov x8, #172\nsvc #0\ncmp x0, #1\ncset x0, ne\n"
        "mov x8, #94\nsvc #0\nbrk #0\n");
