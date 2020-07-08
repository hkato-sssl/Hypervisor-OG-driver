/*
 * hvc_p128_asm.S
 *
 * (C) 2020 Hidekazu Kato
 */

#define HVC_IMM		1

#define ip0		x16
#define ip1		x17

	.global		hvc_p128_read_u16
	.global		hvc_p128_read_u32
	.global		hvc_p128_send_asm
	.global		hvc_p128_receive_asm

	.section	".text.hvc_p128_read_u16", "ax", %progbits
	.type		hvc_p128_read_u16, %function
	.balign		4
	/*
	 * int hvc_p128_read_u16(u64 ip0, u16 *p);
	 */
hvc_p128_read_u16:
	str		x1,  [sp, #-16]!
	mov		ip0, x0

	hvc		#HVC_IMM

	ldr		ip1, [sp], #16
	cbnz		x0,  0f
	strh		w1,  [ip1]
0:
	ret

	.section	".text.hvc_p128_read_u32", "ax", %progbits
	.type		hvc_p128_read_u32, %function
	.balign		4
	/*
	 * int hvc_p128_read_u32(u64 ip0, u32 *p);
	 */
hvc_p128_read_u32:
	str		x1,  [sp, #-16]!
	mov		ip0, x0

	hvc		#HVC_IMM

	ldr		ip1, [sp], #16
	cbnz		x0,  0f
	str		w1,  [ip1]
0:
	ret

	.section	".text.hvc_p128_send_asm", "ax", %progbits
	.type		hvc_p128_send_asm, %function
	.balign		4
	/*
	 * int hvc_p128_send_asm(u64 ip0, const char *buff);
	 */
hvc_p128_send_asm:
	mov		ip0, x0
	ldp		x2,  x3,  [x1, #(2 * 8)] 
	ldp		x4,  x5,  [x1, #(4 * 8)] 
	ldp		x6,  x7,  [x1, #(6 * 8)] 
	ldp		x8,  x9,  [x1, #(8 * 8)] 
	ldp		x10, x11, [x1, #(10 * 8)] 
	ldp		x12, x13, [x1, #(12 * 8)] 
	ldp		x14, x15, [x1, #(14 * 8)] 
	ldp		x0,  x1,  [x1]

	hvc		#HVC_IMM

	ret

	.section	".text.hvc_p128_receive_asm", "ax", %progbits
	.type		hvc_p128_receive_asm, %function
	.balign		4
	/*
	 * int hvc_p128_receive_asm(u64 ip0, const char *buff);
	 */
hvc_p128_receive_asm:
	mov		ip0, x0
	str		x1,  [sp, #-16]!

	hvc		#HVC_IMM

	ldr		ip1, [sp], #16
	cbnz		x0,  0f
	stp		x1,  x2,  [ip1]
	stp		x3,  x4,  [ip1, #(2 * 8)]
	stp		x5,  x6,  [ip1, #(4 * 8)]
	stp		x7,  x8,  [ip1, #(6 * 8)]
	stp		x9,  x10, [ip1, #(8 * 8)]
	stp		x11, x12, [ip1, #(10 * 8)]
	stp		x13, x14, [ip1, #(12 * 8)]
	stp		x15, ip0, [ip1, #(14 * 8)]
0:
	ret

	.end

