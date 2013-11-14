set -e
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "Running un-installation script from $DIR"

echo "Mounting system partition in read-write mode"
mount -wo remount /system
set +e
ls /system/lib/hw/audio.original_primary.* &> /dev/null
installed=$?
set -e

if [ $installed != 0 ]
then
    echo "Copies of audio drivers not found. Already uninstalled?"
    exit 1
fi

echo "Removing SCR audio driver"
rm /system/lib/hw/audio.primary.default.so || true

echo "Restoring original audio drivers"

for file in /system/lib/hw/audio.original_primary.*
do
    new_name=${file//\.original_primary\./\.primary\.}
    echo "    $file => $new_name"
    mv $file $new_name
    chmod 644 $new_name
done

if [ -e /system/etc/audio_policy.conf.back ]
then
echo "Restoring original audio policy file"
rm /system/etc/audio_policy.conf || true
cp /system/etc/audio_policy.conf.back /system/etc/audio_policy.conf
chmod 644 /system/etc/audio_policy.conf
fi

echo "Done"
