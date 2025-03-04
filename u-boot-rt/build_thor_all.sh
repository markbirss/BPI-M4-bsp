mkdir -p DVRBOOT_OUT/hw_setting

make mrproper; make rtd161x_qa_rtk_defconfig;

########### Build Thor A00 RTK #############
CHIP_TYPE=0000
HWSETTING_DIR=examples/flash_writer_nv/hw_setting/rtd161x/demo/$CHIP_TYPE
BUILD_HWSETTING_LIST=`ls $HWSETTING_DIR`

for hwsetting in $BUILD_HWSETTING_LIST
do
	hwsetting=`echo $hwsetting | cut -d '.' -f 1`
	echo %%%%%%%% RTD1619 -- $CHIP_TYPE -- $hwsetting %%%%%%
	if [[ $hwsetting == *"NAND"* ]]; then
		echo "NAND hwsetting skip"
		continue
	fi

	#Build the normal version
	make Board_HWSETTING=$hwsetting CHIP_TYPE=$CHIP_TYPE
	cp ./examples/flash_writer_nv/hw_setting/out/${hwsetting}_final.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-$hwsetting.bin
	cp ./examples/flash_writer_nv/Bind/uda_emmc.bind.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-uda_emmc-$hwsetting.bin
	cp ./examples/flash_writer_nv/Bind/boot_emmc.bind.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-boot_emmc-$hwsetting.bin
	cp ./examples/flash_writer_nv/dvrboot.exe.bin ./DVRBOOT_OUT/A00-$hwsetting.bin

	#Build the drm version
	cp ./examples/flash_writer_nv/image/tee_os/$CHIP_TYPE/fsbl-os-00.00.bin.enlarge ./examples/flash_writer_nv/bootimage/rtd161x/secure-os-00.00.bin
	make Board_HWSETTING=$hwsetting CHIP_TYPE=$CHIP_TYPE PRJ=161x_force_emmc_rtk_drm
	cp ./examples/flash_writer_nv/hw_setting/out/${hwsetting}_final.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-$hwsetting.bin
	cp ./examples/flash_writer_nv/dvrboot.exe.bin ./DVRBOOT_OUT/A00-$hwsetting-drm.bin

	#Reset to normal version
	cp ./examples/flash_writer_nv/image/tee_os/$CHIP_TYPE/fsbl-os-00.00.bin.slim ./examples/flash_writer_nv/bootimage/rtd161x/secure-os-00.00.bin
done


########### Build Thor A01 RTK #############
CHIP_TYPE=0001
HWSETTING_DIR=examples/flash_writer_nv_A01/hw_setting/rtd161x/demo/$CHIP_TYPE
BUILD_HWSETTING_LIST=`ls $HWSETTING_DIR`

for hwsetting in $BUILD_HWSETTING_LIST
do
	hwsetting=`echo $hwsetting | cut -d '.' -f 1`
	echo %%%%%%%% RTD1619 -- $CHIP_TYPE -- $hwsetting %%%%%%
	if [[ $hwsetting == *"NAND"* ]]; then
		echo "NAND hwsetting skip"
		continue
	fi

	#Build the normal version
	make Board_HWSETTING=$hwsetting CHIP_TYPE=$CHIP_TYPE
	cp ./examples/flash_writer_nv_A01/hw_setting/out/${hwsetting}_final.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-$hwsetting.bin
	cp ./examples/flash_writer_nv_A01/Bind/uda_emmc.bind.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-uda_emmc-$hwsetting.bin
	cp ./examples/flash_writer_nv_A01/Bind/boot_emmc.bind.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-boot_emmc-$hwsetting.bin
	cp ./examples/flash_writer_nv_A01/dvrboot.exe.bin ./DVRBOOT_OUT/A01-$hwsetting.bin

	#Build the drm version
	cp ./examples/flash_writer_nv_A01/image/tee_os/$CHIP_TYPE/fsbl-os-00.00.bin.enlarge ./examples/flash_writer_nv_A01/bootimage/rtd161x/secure-os-00.00.bin
	make Board_HWSETTING=$hwsetting CHIP_TYPE=$CHIP_TYPE PRJ=161x_force_emmc_rtk_drm
	cp ./examples/flash_writer_nv_A01/hw_setting/out/${hwsetting}_final.bin ./DVRBOOT_OUT/hw_setting/$CHIP_TYPE-$hwsetting.bin
	cp ./examples/flash_writer_nv_A01/dvrboot.exe.bin ./DVRBOOT_OUT/A01-$hwsetting-drm.bin

	#Reset to normal version
	cp ./examples/flash_writer_nv_A01/image/tee_os/$CHIP_TYPE/fsbl-os-00.00.bin.slim ./examples/flash_writer_nv_A01/bootimage/rtd161x/secure-os-00.00.bin
done

