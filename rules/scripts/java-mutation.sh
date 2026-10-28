#!/usr/bin/env bash
# Run PIT mutation testing through Maven or Gradle and enforce the configured
# efficacy score from the generated mutations.xml.
#
# Usage: java-mutation.sh <src_dir> <min_ratio>
set -euo pipefail

src_dir=$1
min_ratio=$2

if [[ -f "$src_dir/pom.xml" ]]; then
  (cd "$src_dir" && mvn -q test-compile org.pitest:pitest-maven:mutationCoverage \
    -DoutputFormats=XML -DtimestampedReports=false)
  report=$(find "$src_dir/target/pit-reports" -name mutations.xml -print -quit 2>/dev/null || true)
elif [[ -f "$src_dir/build.gradle" || -f "$src_dir/build.gradle.kts" ]]; then
  (cd "$src_dir" && ./gradlew --no-daemon pitest)
  report=$(find "$src_dir/build/reports" -name mutations.xml -print -quit 2>/dev/null || true)
else
  echo "mutation: no supported Java build (pom.xml or build.gradle)" >&2
  exit 2
fi

if [[ -z "$report" || ! -f "$report" ]]; then
  echo "mutation: PIT mutations.xml was not produced" >&2
  exit 1
fi
killed=$(grep -Eo "status=['\"']KILLED['\"']" "$report" | wc -l || true)
survived=$(grep -Eo "status=['\"']SURVIVED['\"']" "$report" | wc -l || true)
total=$((killed + survived))
if ((total == 0)); then
  score=100
else
  score=$((100 * killed / total))
fi
required=$(awk -v value="$min_ratio" 'BEGIN { printf "%.2f", value * 100 }')
awk -v score="$score" -v minimum="$required" 'BEGIN {
  printf "mutation efficacy: %.2f%%, minimum %.2f%%\n", score, minimum
  exit !(score + 0 >= minimum + 0)
}'
