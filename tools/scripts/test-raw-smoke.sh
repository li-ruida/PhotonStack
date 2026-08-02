#!/usr/bin/env bash
set -euo pipefail

raw_path="${1:-}"
if [[ -z "${raw_path}" ]]; then
  echo "Usage: tools/scripts/test-raw-smoke.sh <raw-file>" >&2
  exit 2
fi

if [[ ! -f "${raw_path}" ]]; then
  echo "RAW file does not exist: ${raw_path}" >&2
  exit 2
fi

if [[ -n "${PHOTONSTACK_CLI:-}" ]]; then
  cli="${PHOTONSTACK_CLI}"
elif [[ -x "build/apps/PhotonStackCLI/photonstack" ]]; then
  cli="build/apps/PhotonStackCLI/photonstack"
else
  cli="build/debug/apps/PhotonStackCLI/photonstack"
fi
if [[ ! -x "${cli}" ]]; then
  echo "PhotonStack CLI not found or not executable: ${cli}" >&2
  echo "Build it first with: tools/scripts/test.sh debug" >&2
  exit 2
fi

assert_non_black_preview() {
  local image_path="$1"
  local maximum
  maximum="$("${cli}" histogram --input "${image_path}" | plutil -extract maximum raw -o - -)"
  if ! awk -v value="${maximum}" 'BEGIN { exit !(value > 0) }'; then
    echo "RAW preview is empty or black: ${image_path}" >&2
    exit 1
  fi
}

assert_raw_decode_report() {
  local report="$1"
  local backend fallback fallback_code fallback_message
  backend="$(printf '%s' "${report}" | plutil -extract decodeBackend raw -o - -)"
  fallback="$(printf '%s' "${report}" | plutil -extract decodeFallback raw -o - -)"
  fallback_code="$(printf '%s' "${report}" | plutil -extract decodeFallbackCode raw -o - -)"
  fallback_message="$(printf '%s' "${report}" | plutil -extract decodeFallbackMessage raw -o - -)"
  case "${backend}" in
    apple-raw)
      if [[ "${fallback}" != "false" || -n "${fallback_code}" || -n "${fallback_message}" ]]; then
        echo "Apple RAW decode report contains contradictory fallback metadata" >&2
        exit 1
      fi
      ;;
    imageio)
      if [[ "${fallback}" != "true" || -z "${fallback_code}" || -z "${fallback_message}" ]]; then
        echo "ImageIO RAW fallback report is missing its failure reason" >&2
        exit 1
      fi
      ;;
    *)
      echo "Unexpected RAW decode backend: ${backend}" >&2
      exit 1
      ;;
  esac
}

tmp_dir="$(mktemp -d /tmp/photonstack-raw-smoke.XXXXXX)"
trap 'rm -rf "${tmp_dir}"' EXIT

preview_base="${tmp_dir}/preview-base.png"
preview_bright="${tmp_dir}/preview-bright.png"
preview_manual="${tmp_dir}/preview-manual.png"
preview_linear="${tmp_dir}/preview-linear.png"
preview_srgb="${tmp_dir}/preview-srgb.png"
preview_fast="${tmp_dir}/preview-fast.png"
preview_high="${tmp_dir}/preview-high.png"
converted_tiff="${tmp_dir}/converted.tiff"

"${cli}" raw inspect "${raw_path}" >/dev/null

preview_base_report="$("${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_base}" \
  --width 640 \
  --raw-white-balance camera \
  --raw-exposure-bias 0 \
  --raw-black-level camera \
  --raw-demosaic balanced \
  --raw-linear off)"
assert_raw_decode_report "${preview_base_report}"

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_bright}" \
  --width 640 \
  --raw-white-balance camera \
  --raw-exposure-bias 1 \
  --raw-black-level camera \
  --raw-demosaic balanced \
  --raw-linear off >/dev/null

if cmp -s "${preview_base}" "${preview_bright}"; then
  echo "Exposure bias did not change RAW preview output" >&2
  exit 1
fi

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_manual}" \
  --width 640 \
  --raw-white-balance manual \
  --raw-temperature 5000 \
  --raw-tint 10 \
  --raw-exposure-bias 0.5 \
  --raw-black-level manual \
  --raw-black-value 0.002 \
  --raw-demosaic high \
  --raw-linear off >/dev/null

if cmp -s "${preview_base}" "${preview_manual}"; then
  echo "Manual white balance and black level did not change RAW preview output" >&2
  exit 1
fi

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_linear}" \
  --width 640 \
  --raw-white-balance auto \
  --raw-exposure-bias 0 \
  --raw-black-level auto \
  --raw-demosaic high \
  --raw-linear on >/dev/null

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_srgb}" \
  --width 640 \
  --raw-white-balance auto \
  --raw-exposure-bias 0 \
  --raw-black-level auto \
  --raw-demosaic high \
  --raw-linear off >/dev/null

if cmp -s "${preview_linear}" "${preview_srgb}"; then
  echo "RAW linear on/off did not change preview output" >&2
  exit 1
fi

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_fast}" \
  --width 640 \
  --raw-white-balance camera \
  --raw-exposure-bias 0 \
  --raw-black-level camera \
  --raw-demosaic fast \
  --raw-linear on >/dev/null

"${cli}" preview \
  --input "${raw_path}" \
  --output "${preview_high}" \
  --width 640 \
  --raw-white-balance camera \
  --raw-exposure-bias 0 \
  --raw-black-level camera \
  --raw-demosaic high \
  --raw-linear on >/dev/null

if [[ ! -s "${preview_fast}" || ! -s "${preview_high}" ]]; then
  echo "RAW fast/full-quality decode did not produce both preview outputs" >&2
  exit 1
fi

for preview in \
  "${preview_base}" \
  "${preview_bright}" \
  "${preview_manual}" \
  "${preview_linear}" \
  "${preview_srgb}" \
  "${preview_fast}" \
  "${preview_high}"; do
  assert_non_black_preview "${preview}"
done

convert_report="$("${cli}" raw convert \
  --input "${raw_path}" \
  --output "${converted_tiff}" \
  --bit-depth 16 \
  --color-space linear-srgb \
  --raw-white-balance auto \
  --raw-exposure-bias 0.5 \
  --raw-black-level auto \
  --raw-demosaic high \
  --raw-linear on)"
assert_raw_decode_report "${convert_report}"

if [[ ! -s "${converted_tiff}" ]]; then
  echo "RAW convert did not produce a TIFF output" >&2
  exit 1
fi

echo "RAW smoke test passed: ${raw_path}"
