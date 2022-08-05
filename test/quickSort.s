.arch armv7ve
.arm
.fpu neon
.text
.global main
.section .text
.memset:
	push {r4}
	mov r2,#0
	mov r3,#0
	mov r4,#8
.memset8:
	sub r1,r1,#8
	cmp r1,#0
	blt .memset4
	strd r2,r3,[r0],r4
	bne .memset8
	b .memset_end
.memset4:
	str r2,[r0],#4
.memset_end:
	pop {r4}
	bx lr
.init:
.L0:
	bx lr
@ spilled Size: 0
@ stack Size: 4
main:
	push {lr}
	sub sp,sp, #4
	bl .init
.L1:
	mov r0,#0
	str r0,[sp,#0]
	mov r0,#3
	bl _sysy_starttime
	mov r0,#4
	bl _sysy_stoptime
	mov r0,#0
	add sp,sp, #4
	pop {pc}
