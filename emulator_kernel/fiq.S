#include <linux/linkage.h>
#include <asm/assembler.h>
	.text

	.global nctuss_fiq_end
ENTRY(nctuss_fiq_start)

/* wait for my_ipi reigsters to be cleared */
	cmp	r8, #0
	beq cpu00
	cmp	r8, #1
	beq cpu11

cpu00:
	ldr	r13, [r9]
	teq	r13, #0
	wfene
	bne cpu00
	b done_clearr
cpu11:
	ldr	r13, [r9, #4]
	teq	r13, #0
	wfene
	bne cpu11
	b done_clearr

done_clearr:

	subs	pc, lr, #4




	/* save current counter value */
//	ldr	r13, [r11]
//	str	r13, [r12]

	/* Check that OS_pause is really ==1 */
//	ldr	r13, [r10]
//	cmp	r13, #1
//	bne	blink_my_ipi_LED

wait_OS_pause_1:
	ldr	r13, [r10]
	cmp	r13, #1
	bne	wait_OS_pause_1

/* set active CPU led */
	cmp	r8, #0
	beq cpu0_led
	cmp	r8, #1
	beq cpu1_led

cpu0_led:
	mov	r13, #1
	str	r13, [r9, #16]

	b set_LED_done
cpu1_led:
	mov	r13, #1
	str	r13, [r9, #20]

	b set_LED_done

set_LED_done:

	/* Set OS_pause=2 */

	dmb
	dsb
	mov	r13, #2
	str	r13, [r10]
	dmb
	dsb


	/* Check OS resume */
loop:
	mov	r13, #1000
delay:
	sub	r13, #1
	cmp	r13, #0
	bne	delay
	
	ldr	r13, [r10]
	dmb
	dsb
	cmp	r13, #0
	bne loop


	/* Set OS_pause=2 */
	mov	r13, #2
	str	r13, [r10]
	dmb
	dsb


/* clear my_ipi reigsters */
	cmp	r8, #0
	beq cpu0
	cmp	r8, #1
	beq cpu1

cpu0:
	mov	r13, #0
	str	r13, [r9]
	b done_clear
cpu1:
	mov	r13, #0
	str	r13, [r9, #4]
	b done_clear

done_clear:

	dmb
	dsb

	subs	pc, lr, #4

blink_my_ipi_LED:
	mov	r13, #0
	ldr	r13, [r9]
	nop
	nop
	mov	r13, #1
	ldr	r13, [r9]
	nop
	nop
	b blink_my_ipi_LED


nctuss_fiq_end:
