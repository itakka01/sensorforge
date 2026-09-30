#!/bin/bash

set -e

cd "$(dirname "$0")"

echo "----------------------------------------"
echo "Sensorforge GitHub Auto-Pull"
echo "----------------------------------------"

git status

echo "Pulling latest version..."
git pull --ff-only origin main

echo "----------------------------------------"
echo "Fertig! Lokale Version ist aktuell."
echo "----------------------------------------"


