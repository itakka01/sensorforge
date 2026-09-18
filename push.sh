#!/bin/bash

# Usage:
#   ./push.sh "release motion detection_v2"

# Commit message from argument
MSG="$1"

if [ -z "$MSG" ]; then
    echo "Fehler: Bitte Commit-Message angeben."
    echo "Beispiel: ./push.sh \"release motion detection_v2\""
    exit 1
fi

echo "----------------------------------------"
echo "Sensorforge GitHub Auto-Push"
echo "Commit-Message: $MSG"
echo "----------------------------------------"

# Add all new/changed files
git add .

# Show status
git status

# Commit
git commit -m "$MSG"

# Push
git push origin main

echo "----------------------------------------"
echo "Fertig! Alles wurde nach GitHub gepusht."
echo "----------------------------------------"
