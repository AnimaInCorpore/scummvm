; Resident wavetable gate. Initial bank upload is separately profiled.
; Steady rendering sends controls only, with pitch/volume changing per block.
        include "xbios.i"
        include "image.i"
        include "config.i"
        text
start:
        Locksnd
        cmpi.l  #1,d0
        bne     failed
        Dsp_ExecBoot mix_boot,#MIX_BOOT_WORDS,#3
        Supexec render
        Dsp_Unlock
        Unlocksnd
        Fcreate filename,#0
        tst.l   d0
        bmi     failed
        move.w  d0,handle
        Fwrite handle,#OUTPUT_BYTES,output
        cmp.l   #OUTPUT_BYTES,d0
        bne     failed
        Fclose handle
        Pterm0
failed:
        move.w  #1,-(sp)
        move.w  #$4c,-(sp)
        trap    #1
render:
        movem.l d2-d7/a2-a6,-(sp)
        lea     $ffffa202,a5
        lea     $ffffa204,a6
        lea     startup,a0
        move.l  #STARTUP_WORDS,d4
        move.l  #$135780,d7
load:
        btst    #1,(a5)
        beq.s   load
        move.l  (a0)+,(a6)
        subq.l  #1,d4
        bne.s   load
ready:
        btst    #0,(a5)
        beq.s   ready
        move.l  (a6),d0
        move.l  #$246880,d7
        lea     controls,a0
        lea     output,a4
        move.w  #BLOCKS-1,d6
        move.l  #$13579b,d7
period:
        move.l  (a0)+,d4
        subq.w  #1,d4
send:
        btst    #1,(a5)
        beq.s   send
        move.l  (a0)+,(a6)
        dbra    d4,send
        move.w  #FRAMES*2-1,d4
receive:
        btst    #0,(a5)
        beq.s   receive
        move.l  (a6),d0
        move.w  d0,(a4)+
        dbra    d4,receive
        dbra    d6,period
        move.l  #$2468ac,d7
        nop
        movem.l (sp)+,d2-d7/a2-a6
        rts
        data
filename: dc.b 'OUTPUT.RAW',0
        even
startup:
        incbin "START.RAW"
controls:
        incbin "CTRL.RAW"
        bss
handle: ds.w 1
output: ds.b OUTPUT_BYTES
        end
