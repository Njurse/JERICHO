@echo off
ffmpeg -y -i "%~1" -ac 1 -ar 11025 -sample_fmt s16 "%~dpn1.d2car.wav"
echo Done: %~dpn1.d2car.wav
pause
