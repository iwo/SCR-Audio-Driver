set -e
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "Running installation script from $DIR"

echo "Mounting system partition in read-write mode"
mount -wo remount /system
set +e
ls /system/lib/hw/audio.original_primary.* &> /dev/null
installed=$?
set -e

if [ $installed != 0 ]
then
    echo "Moving original audio drivers"
    for file in /system/lib/hw/audio.primary.*
    do
        new_name=${file//primary/original_primary}
        echo "    $file => $new_name"
        mv $file $new_name
    done
else
    echo "Original drivers already backed up"
fi

echo "Copying SCR audio driver"
cp $DIR/audio.primary.default.so /system/lib/hw/audio.primary.default.so
chmod 644 /system/lib/hw/audio.primary.default.so

if [ -e $DIR/audio_policy.conf ]
then
    echo "Installing audio policy file"
    if [ -e /system/etc/audio_policy.conf ] && [ ! -e /system/etc/audio_policy.conf.back ]
    then
        echo "Moving original audio policy file /system/etc/audio_policy.conf to /system/etc/audio_policy.conf.back"
        mv /system/etc/audio_policy.conf /system/etc/audio_policy.conf.back
    fi
    if [ -e /vendor/etc/audio_policy.conf ] && [ ! -e /vendor/etc/audio_policy.conf.back ]
    then
        echo "Moving original audio policy file /vendor/etc/audio_policy.conf to /vendor/etc/audio_policy.conf.back"
        mv /vendor/etc/audio_policy.conf /vendor/etc/audio_policy.conf.back
    fi
    cp $DIR/audio_policy.conf /system/etc/audio_policy.conf
    chmod 644 /system/etc/audio_policy.conf
fi

echo "Done"
