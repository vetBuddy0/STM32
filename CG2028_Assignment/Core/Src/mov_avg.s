/*
 * mov_avg.s
 *
 * CG2028 Assignment starter file.
 *
 * ewma_filter: one step of the integer EWMA filter. main.c calls it for each
 * of the 6 axes every 20 ms (and 3 more times for the pose tracker).
 */
.syntax unified
.cpu cortex-m4              @ M4 has hardware SDIV
.thumb
.global ewma_filter
.type ewma_filter, %function

.text
.align 2

@ CG2028 Assignment
@ (c) ECE NUS
@ Chakraborty Basudeb:  (A0288113E)
@ Reyas Mohideen Fawaz: (A0327682W)
@
@ Function prototype:
@   int ewma_filter(int new_data, int old_output, int alpha_percent);
@
@ ARM calling convention:
@   R0 = new_data       (signed integer sensor sample)
@   R1 = old_output     (previous filtered output)
@   R2 = alpha_percent  (integer from 0 to 100)
@   Return R0 = (alpha_percent * new_data
@                + (100 - alpha_percent) * old_output) / 100
@
@ Notes:
@ - Use signed integer arithmetic.
@ - Integer division must truncate towards zero, matching C integer division.
@ - Preserve all callee-saved registers that you use (R4-R11).
@ - Do not call a C helper function and do not use floating-point instructions.
@
@ Registers:
@   R3  sum
@   R4  100 - alpha, then 100 (callee-saved, so pushed/popped)
@
@ e.g. new=1000, old=0, alpha=25:
@   25000 + 75*0 = 25000, 25000/100 = 250
@
@ Write your program from here.
ewma_filter:
    PUSH {r4-r7, lr}        @ save R4-R7 + return address
    MUL  r3, r0, r2         @ r3 = alpha * new
    RSB  r4, r2, #100       @ r4 = 100 - alpha
    MLA  r3, r4, r1, r3     @ r3 += (100 - alpha) * old
    MOV  r4, #100
    SDIV r0, r3, r4         @ signed divide, truncates toward 0 same as C
    POP  {r4-r7, pc}        @ restore and return

.size ewma_filter, .-ewma_filter
