#!/bin/bash

cd /app/game/data/
AppVersion=$(cat /app/share/appdata/io.github.opendriver2.Redriver2.appdata.xml | awk  -F'"' 'NR==15 {print $2":"$4}')

function importDefaultData {
  cp -rf `ls /app/game/data/ | grep -v '^config.ini$'` /var/data/
	if [ ! -f /var/data/config.ini ]; then
	  echo "Config file not found, importing default config."
	  cp /app/game/data/config.ini /var/data/config.ini
	fi
	echo "$AppVersion" > /var/cache/JERICHO.cache
}

if [ ! -d /var/data/DRIVER2 ]; then
	zenity --error --no-wrap --text="`printf "DRIVER2 files are missing! Please provide DRIVER2 folder in:\n ${HOME}/.var/app/io.github.opendriver2.Redriver2/data"`"
	exit 0
fi

if [ ! -f /var/cache/JERICHO.cache ]; then
  echo "Cache not found, overwriting files."
	importDefaultData
fi

if [[ $(< /var/cache/JERICHO.cache) != "$AppVersion" ]]; then
  echo "JERICHO version not matching, overwriting files."
  importDefaultData
fi

args=""
while [[ "$#" -gt 0 ]]; do
	args+="$1 "
	shift
done

cd /app/game/bin/
case $JERICHO_BUILD in
	release) ./JERICHO $([ ! -z "$args" ] && echo "$args"); shift;;
	dev) ./JERICHO_dev $([ ! -z "$args" ] && echo "$args"); shift;;
	debug) ./JERICHO_dbg $([ ! -z "$args" ] && echo "$args"); shift;;
	*) ./JERICHO $([ ! -z "$args" ] && echo "$args"); # Fallback
esac
