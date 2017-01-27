Title: Jasontek_f103rb demo

Description:
samples\basic\button    demo 
samples\shell           demo
samples\synchronization demo
samples\philosophers    demo

can run in jasontek_f103rb board with MACRO NUCLEO_F103RB

--------------------------------------------------------------------------------

Building and Running Project:
It can be built for a nucleo_f103rb board as follows:

    make menuconfig BOARD=jasontek_f103rb ARCH=arm
    make BOARD=jasontek_f103rb ARCH=arm -jN
    
if you don't want to type BOARD ARCH, you can export these variants to shell.

The code may need adaption before running the code on another board.

--------------------------------------------------------------------------------

Troubleshooting:

Problems caused by out-dated project information can be addressed by
issuing one of the following commands then rebuilding the project:

    make clean          # discard results of previous builds
                        # but keep existing configuration info
or
    make pristine       # discard results of previous builds
                        # and restore pre-defined configuration info

--------------------------------------------------------------------------------
