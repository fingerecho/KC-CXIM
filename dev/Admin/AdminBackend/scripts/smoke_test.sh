#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
USERNAME="${CX_ADMIN_USER:-admin}"
PASSWORD="${CX_ADMIN_PASS:-Admin@123}"
BODY_FILE="${TMPDIR:-/tmp}/cx_admin_smoke_body.json"

pass() {
    printf '[smoke] PASS %s\n' "$1"
}

fail() {
    printf '[smoke] FAIL %s\n' "$1" >&2
    exit 1
}

request() {
    local method="$1"
    local path="$2"
    local expected="$3"
    local data="${4:-}"
    local token="${5:-}"
    local args=(
        -s
        -o "$BODY_FILE"
        -w '%{http_code}'
        -X "$method"
        "${BASE_URL}${path}"
    )

    if [[ -n "$data" ]]; then
        args+=(-H 'Content-Type: application/json' -d "$data")
    fi
    if [[ -n "$token" ]]; then
        args+=(-H "Authorization: Bearer ${token}")
    fi

    local code
    code="$(curl "${args[@]}")"
    if [[ "$code" != "$expected" ]]; then
        printf '[smoke] expected %s, got %s: %s %s\n' \
            "$expected" "$code" "$method" "$path" >&2
        cat "$BODY_FILE" >&2
        printf '\n' >&2
        fail "$method $path"
    fi
    pass "$method $path -> $code"
}

printf '[smoke] base_url=%s\n' "$BASE_URL"

request GET /healthz 200
request POST /admin/api/auth/login 200 \
    "{\"username\":\"${USERNAME}\",\"password\":\"${PASSWORD}\"}"

TOKEN="$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$BODY_FILE")"
if [[ -z "$TOKEN" ]]; then
    cat "$BODY_FILE" >&2
    fail "login token missing"
fi

request GET '/admin/api/users?page=1&page_size=8' 200 '' "$TOKEN"
request GET '/admin/api/users?page=0' 400 '' "$TOKEN"
request GET /admin/api/users/999999999 404 '' "$TOKEN"
request GET /admin/api/users 401
request GET /admin/api/stats/overview 200 '' "$TOKEN"
request GET '/admin/api/stats/trend?days=30' 200 '' "$TOKEN"
request GET /admin/api/auth/me 200 '' "$TOKEN"

printf '[smoke] ALL PASS\n'
