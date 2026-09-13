; SSI -> DAC + DMA recording. Captures the actual Falcon digital output path.
        include "xbios.i"
        include "image.i"
        include "config.i"
        text
start:
        Locksnd
        cmpi.l  #1,d0
        bne     failed
        Buffoper #0
        Sndstatus #1
        Setmode #1
        Settracks #0,#0
        Setmontracks #0
        Dsptristate #1,#1
        Devconnect #1,#9,#0,#3,#1
        Setbuffer #1,#output,#output_end
        Dsp_ExecBoot mix_boot,#MIX_BOOT_WORDS,#3
        Buffoper #4
        Supexec render
        Vsync
        Vsync
        Buffoper #0
        Dsptristate #0,#0
        Devconnect #1,#0,#0,#3,#1
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
        Fcreate statsname,#0
        move.w  d0,handle
        Fwrite handle,#16,stats
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
ack:
        btst    #0,(a5)
        beq.s   ack
        move.l  (a6),d0
        and.l   #$ffffff,d0
        cmp.l   #$455252,d0
        beq.s   read_stats
        dbra    d6,period
read_stats:
        lea     stats,a4
        move.w  #3,d4
drain:
        btst    #0,(a5)
        beq.s   drain
        move.l  (a6),d0
        and.l   #$ffffff,d0
        move.l  d0,(a4)+
        dbra    d4,drain
        move.l  #$2468ac,d7
        nop
        movem.l (sp)+,d2-d7/a2-a6
        rts
        data
filename: dc.b 'OUTPUT.RAW',0
statsname: dc.b 'STATS.RAW',0
        even
startup: incbin "START.RAW"
controls: incbin "CTRL.RAW"
        bss
handle: ds.w 1
stats: ds.l 4
output: ds.b OUTPUT_BYTES
output_end:
        end
