Title: Boot Time Measurement

Description:

BootTime measures the time:
   a) from system reset to kernel start (crt0.s's __start)
   b) from kernel start to begin of main()
   c) from kernel start to begin of first task
   d) from kernel start to when microkernel's main task goes immediately idle

The project can be built using one of the following three configurations:

best
-------
 - Disables most features
 - Provides best case boot measurement

default
-------
 - Default configuration options
 - Provides typical boot measurement

worst
-------
 - Enables most features.
 - Provides worst case boot measurement

--------------------------------------------------------------------------------

Building and Running Project:

This microkernel project outputs to the console.  It can be built and executed
on QEMU in three possibile configurations as follows:

    make BOOTTIME_QUALIFIER=best qemu

    make BOOTTIME_QUALIFIER=default qemu

    make BOOTTIME_QUALIFIER=worst qemu

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

Sample Output:

tc_start() - Boot Time Measurement
MicroKernel Boot Result: Clock Frequency: 20 MHz
__start       : 377787 cycles, 18889 us
_start->main(): 3915 cycles, 195 us
_start->task  : 5898 cycles, 294 us
_start->idle  : 6399 cycles, 319 us
Boot Time Measurement finished
===================================================================
PASS - bootTimeTask.
===================================================================
PROJECT EXECUTION SUCCESSFUL
