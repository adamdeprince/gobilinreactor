// Linked as an independent, relocation-free image. No Android code executes
// after this entry, and no helper reads or writes the thread-pointer register.
.text
.global stub_start
.hidden stub_start
.type stub_start, %function
stub_start:
    ldr x9, [x0]
    mov sp, x9
    bl stub_boot_main
    brk #0

.global stub_syscall
.hidden stub_syscall
.type stub_syscall, %function
stub_syscall:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
.global stub_syscall_instruction
.hidden stub_syscall_instruction
stub_syscall_instruction:
    svc #0
.global stub_syscall_return
.hidden stub_syscall_return
stub_syscall_return:
    ret

.global stub_restore
.hidden stub_restore
.type stub_restore, %function
stub_restore:
    mov x0, #139
    b stub_syscall

.global stub_resume_frame
.hidden stub_resume_frame
.type stub_resume_frame, %function
stub_resume_frame:
    mov sp, x0
    b stub_restore

.section .note.GNU-stack,"",%progbits
