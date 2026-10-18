#!/bin/sh
# Renders the face test video for every language: Seed-san, Vulkan, with the face track
# and the matching clip's audio muxed in. Output: renders/face_<lang>_seed_vk.mp4
set -e
cd "$(dirname "$0")/.."
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
for entry in "en 14.68" "ja 20.08" "ko 19.92" "zh-CN 17.62" "zh-TW 18.70" "zh-HK 20.31"; do
  set -- $entry
  lang=$1; secs=$2
  xvfb-run -a ./build/ukiyo --headless --backend vk --size 960x540 --fps 30 --seconds "$secs" \
    --turns 0 --yaw 0 --distance 1.7 --target-y 1.3 \
    --face "renders/face/face_${lang}.json" \
    --audio "renders/audio/clip_${lang}.mp3" \
    --video "renders/face_${lang}_seed_vk.mp4" | tail -1
done
echo ALL_DONE
