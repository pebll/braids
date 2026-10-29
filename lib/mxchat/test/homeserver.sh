#!/usr/bin/env bash
#
# homeserver.sh
#
# Start a throwaway local Synapse homeserver for mxchat development, with
# test accounts @alice:localhost and @bob:localhost.
#
# Usage: homeserver.sh [start|stop|reset] [DATA_DIR]
#
# Server:   http://localhost:8008 (server name "localhost", .well-known served)
# Accounts: alice / test-alice-pw, bob / test-bob-pw

set -e

CMD="${1:-start}"
DATA="${2:-${TMPDIR:-/tmp}/nchat-mx-synapse}"
NAME="nchat-mx-synapse"
IMAGE="matrixdotorg/synapse:latest"
URL="http://localhost:8008"

stop() {
  docker rm -f "${NAME}" > /dev/null 2>&1 || true
}

if [[ "${CMD}" == "stop" ]]; then
  stop
  exit 0
fi

if [[ "${CMD}" == "reset" ]]; then
  stop
  rm -rf "${DATA}"
fi

if [[ ! -f "${DATA}/homeserver.yaml" ]]; then
  mkdir -p "${DATA}"
  docker run --rm -v "${DATA}:/data" -e SYNAPSE_SERVER_NAME=localhost \
         -e SYNAPSE_REPORT_STATS=no "${IMAGE}" generate
  # bind ipv4 only (ipv6 may be unavailable in containers)
  sed -i "s/^    type: http$/    type: http\n    bind_addresses: ['127.0.0.1']/" "${DATA}/homeserver.yaml"
  cat >> "${DATA}/homeserver.yaml" << 'EOF'
enable_registration: false
serve_server_wellknown: true
public_baseurl: http://localhost:8008/
rc_message: {per_second: 1000, burst_count: 1000}
rc_login:
  address: {per_second: 1000, burst_count: 1000}
  account: {per_second: 1000, burst_count: 1000}
  failed_attempts: {per_second: 1000, burst_count: 1000}
rc_registration: {per_second: 1000, burst_count: 1000}
EOF
  NEW=1
fi

stop
docker run -d --name "${NAME}" --network host -v "${DATA}:/data" "${IMAGE}" > /dev/null

for _ in $(seq 1 60); do
  curl -sf "${URL}/_matrix/client/versions" > /dev/null && break
  sleep 1
done

if [[ "${NEW}" == "1" ]]; then
  for U in alice bob; do
    docker exec "${NAME}" register_new_matrix_user -c /data/homeserver.yaml \
           -u "${U}" -p "test-${U}-pw" --no-admin "${URL}"
  done
fi

echo "synapse running at ${URL} (data ${DATA})"
