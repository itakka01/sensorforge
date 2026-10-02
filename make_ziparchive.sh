#!/bin/bash

outfile="sensorforge_$(date +'%Y-%m-%d_%H-%M-%S').zip"

find . -maxdepth 1 -type f ! -name '*.zip' ! -name '*.pem' ! -name 'config.txt' ! -name "$outfile" -print0 \
  | xargs -0 zip "$outfile"

