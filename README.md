# FIRM-AFL

FIRM-AFL is the first high-throughput greybox fuzzer for IoT firmware. FIRM-AFL addresses two fundamental problems in IoT fuzzing. First, it addresses compatibility issues by enabling fuzzing for POSIX-compatible firmware that can be emulated in a system emulator. Second, it addresses the performance bottleneck caused by system-mode emulation with a novel technique called "augmented process emulation". By combining system-mode emulation and user-mode emulation in a novel way, augmented process emulation provides high compatibility as system-mode emulation and high throughput as user-mode emulation. 

## Publication

Yaowen Zheng, Ali Davanian, Heng Yin, Chengyu Song, Hongsong Zhu, Limin Sun, “FIRM-AFL: High-throughput greybox fuzzing of IoT firmware via augmented process emulation,” in USENIX Security Symposium, 2019.

## Introduction

FIRM-AFL is the first high-throughput greybox fuzzer for IoT firmware. FIRM-AFL addresses two fundamental problems in IoT fuzzing. First, it addresses compatibility issues by enabling fuzzing for POSIX-compatible firmware that can be emulated in a system emulator. Second, it addresses the performance bottleneck caused by system-mode emulation with a novel technique called "augmented process emulation". By combining system-mode emulation and user-mode emulation in a novel way, augmented process emulation provides high compatibility as system-mode emulation and high throughput as user-mode emulation. The overview is show in Figure 1.

<div align=center><img src="https://github.com/zyw-200/FirmAFL/raw/master/image/augmented_process_emulation.png" width=70% height=70% /></div>

<div align=center>Figure 1. Overview of Augmented Process Emulation</div>

&nbsp;

We design and implement FIRM-AFL, an enhancement of AFL for fuzzing IoT firmware. We keep the workflow of AFL intact and replace the user-mode QEMU with augmented process emulation, and the rest of the components remain unchanged. The new workflow is illustrated in Figure 2.

<div align=center><img src="https://github.com/zyw-200/FirmAFL/raw/master/image/overview_of_FirmAFL.png" width=70% height=70% /></div>

<div align=center>Figure 2. Overview of FIRM-AFL</div>

### TriforceAFL_new

		A tool for simulation, dynamic analysis and fuzzing of IoT firmware.
		Combination of TriforceAFL, firmadyne and DECAF.

#### DECAF: upgraded to the newest qemu version 2.10.1
		It is included in qemu_mode/qemu dir. 
		In our case, run ./configure --target-list=mipsel-softmmu
		Run make

#### Firmadyne: we use its custom kernel and libnvram to emulate IoT firmware. 
		cd firmadyne 
		See README in firmadyne and do as it says.(NOTICE: need to set FIRMWARE_DIR in firmadyne.config
		Here, we test DIR-815_FIRMWARE_1.01.ZIP, a router firmware image based on mipsel cpu arch.
		run "../qemu_mode/qemu/qemu-img convert -f raw -O qcow2 ./scratch/2/image.raw ./scratch/2/image.qcow2"		
		Finally, we replace the run.sh in scratch/(num)/ with our modified one (In firmadyne_dev dir).
		


#### TriforceAFL: AFL fuzzing with full-system emulation
		Run make
  


#### Usage:
		cd firmadyne
		Run ./scratch/(num)/run.sh 
		In another terminal, run 'telnet 127.0.0.1 4444', into qemu monitor console.
		FirmFuzzer plugin:
			load_plugin ../qemu_mode/qemu/plugins/callbacktests/callbacktests.so
			do_callbacktests httpd
			do_callbacktests hedwig.cgi
			When firmware system initialization is completed and poll system call is executed, open a Browser, type a request "192.168.0.1/hedwig.cgi" in url, the fuzz process will be started.
		MalScalpel plugin:
			load_plugin ../qemu_mode/qemu/plugins/unpacker/unpacker.so
			trace_by_name mirai.mpsl
			Then, telnet into system "telnet 192.168.0.1" with username "Alphanetworks" and password "wrgnd08_dlob_dir815"
			Run "/FILE_LOAD/mirai.mpsl", the plugin works.


## Related Work

Our system is built on top of TriforceAFL, DECAF, AFL, and Firmadyne.

**TriforceAFL:** AFL/QEMU fuzzing with full-system emulation, https://github.com/nccgroup/TriforceAFL.

**DECAF:** "Make it work, make it right, make it fast: building a platform-neutral whole-system dynamic binary analysis platform", Andrew Henderson, Aravind Prakash, Lok Kwong Yan, Xunchao Hu, Xujiewen Wang, Rundong Zhou, and Heng Yin, to appear in the International Symposium on Software Testing and Analysis (ISSTA'14), San Jose, CA, July 2014. https://github.com/sycurelab/DECAF.

**AFL:** american fuzzy lop (2.52b), http://lcamtuf.coredump.cx/afl/.

**Firmadyne:** Daming D. Chen, Maverick Woo, David Brumley, and Manuel Egele. “Towards automated dynamic analysis for Linux-based embedded firmware,” in Network and Distributed System Security Symposium (NDSS’16), 2016. https://github.com/firmadyne.