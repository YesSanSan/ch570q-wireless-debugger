.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.global g_vectors
g_vectors:
 .word _estack, Reset_Handler
 .word Default_Handler, HardFault_Handler
 .word Default_Handler, Default_Handler, Default_Handler
 .word 0, 0, 0, 0
 .word Default_Handler, Default_Handler, 0, Default_Handler, SysTick_Handler
 .rept 43
 .word Default_Handler
 .endr
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
 ldr r0, =_sidata
 ldr r1, =_sdata
 ldr r2, =_edata
1:
 cmp r1, r2
 bcs 2f
 ldr r3, [r0], #4
 str r3, [r1], #4
 b 1b
2:
 ldr r1, =_sbss
 ldr r2, =_ebss
 movs r3, #0
3:
 cmp r1, r2
 bcs 4f
 str r3, [r1], #4
 b 3b
4:
 bl main
 b .
.size Reset_Handler, .-Reset_Handler
.section .text.Default_Handler,"ax",%progbits
.global Default_Handler
.type Default_Handler,%function
.thumb_func
Default_Handler:
 b .
.section .note.GNU-stack,"",%progbits
