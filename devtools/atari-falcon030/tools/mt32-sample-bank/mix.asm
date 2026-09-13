; Streaming 32-voice feasibility gate. No interpolation, reverb or SSI.
; Each block: VOICES times [left/right gain Q23, 512 signed samples].
; PACKED12 instead receives 256 words containing two signed 12-bit samples.
; Return 1024 stereo words. Blocking host traffic is included in the CPU timer.
        include 'ioequ.inc'
        include 'config.inc'
        org     p:0
        jmp     start
        org     p:$40
start:
        movep   #1,x:m_pbc
        movep   #0,x:m_bcr
        move    #>-1,m0
block:
        move    #>$1000,r0
        clr     a
        do      #1024,clear_done
        move    a1,x:(r0)+
clear_done:
        do      #VOICES,voices_done
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y0
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y1
        move    #>$1000,r0
        if      PACKED12
        do      #256,samples_done
        else
        do      #512,samples_done
        endif
mix_loop:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,x0
        if      PACKED12
        move    x0,y:$20
        move    #>$008000,x1
        mpy     x0,x1,a
        move    #>$fffff0,x1
        and     x1,a
        move    a1,x0
        endif
        move    x:(r0)+,a
        mac     x0,y0,a x:(r0)-,b
        mac     x0,y1,b a1,x:(r0)+
        move    b1,x:(r0)+
        if      PACKED12
        move    y:$20,x0
        move    #>$000800,x1
        mpy     x0,x1,a
        move    a0,x0
        move    #>$008000,x1
        mpy     x0,x1,a
        move    a1,x0
        move    x:(r0)+,a
        mac     x0,y0,a x:(r0)-,b
        mac     x0,y1,b a1,x:(r0)+
        move    b1,x:(r0)+
        endif
samples_done:
        nop
voices_done:
        move    #>$1000,r0
        do      #1024,output_done
        jclr    #1,x:m_hsr,*
        movep   x:(r0)+,x:m_htx
output_done:
        jmp     block
        end
