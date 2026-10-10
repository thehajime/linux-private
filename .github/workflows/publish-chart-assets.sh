#!/usr/bin/env bash
set -euo pipefail

chart_dir=${CHART_DIR:-output/out}
asset_branch=${ASSET_BRANCH:-uml-ci-report-assets}
run_path="runs/${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}"
charts=(lmbench.png iperf3.png tcp-stream.png nginx-um-mmu.png nginx-um-mmu-seccomp.png \
	nginx-um-nommu.png nginx-um-nommu-seccomp.png nginx-um-nommu-sas-seccomp.png \
	nginx-um-nommu-sas-zpoline.png)
available=()

for chart in "${charts[@]}"; do
	if [[ -s "$chart_dir/$chart" ]]; then
		available+=("$chart")
	fi
done

if ((${#available[@]} == 0)); then
	echo "No benchmark charts found; no permalink assets to publish."
	echo "asset_base_url=" >> "$GITHUB_OUTPUT"
	exit 0
fi

gh auth setup-git
repo_url="https://github.com/${GITHUB_REPOSITORY}.git"
asset_dir="$RUNNER_TEMP/uml-ci-report-assets"

for attempt in 1 2 3 4 5; do
	rm -rf "$asset_dir"
	if git ls-remote --exit-code --heads "$repo_url" "refs/heads/$asset_branch" >/dev/null 2>&1; then
		git clone --depth=1 --branch "$asset_branch" "$repo_url" "$asset_dir"
	else
		git init --initial-branch="$asset_branch" "$asset_dir"
		git -C "$asset_dir" remote add origin "$repo_url"
		git -C "$asset_dir" config user.name "github-actions[bot]"
		git -C "$asset_dir" config user.email "41898282+github-actions[bot]@users.noreply.github.com"
	fi

	git -C "$asset_dir" config user.name "github-actions[bot]"
	git -C "$asset_dir" config user.email "41898282+github-actions[bot]@users.noreply.github.com"
	mkdir -p "$asset_dir/$run_path"
	for chart in "${available[@]}"; do
		cp "$chart_dir/$chart" "$asset_dir/$run_path/$chart"
	done
	git -C "$asset_dir" add "$run_path"
	if git -C "$asset_dir" diff --cached --quiet; then
		asset_commit=$(git -C "$asset_dir" rev-parse HEAD)
		echo "asset_base_url=https://raw.githubusercontent.com/${GITHUB_REPOSITORY}/${asset_commit}/${run_path}" >> "$GITHUB_OUTPUT"
		exit 0
	fi
	git -C "$asset_dir" commit -m "ci: save benchmark charts for run ${GITHUB_RUN_ID} attempt ${GITHUB_RUN_ATTEMPT}"

	if git -C "$asset_dir" push origin "HEAD:refs/heads/$asset_branch"; then
		asset_commit=$(git -C "$asset_dir" rev-parse HEAD)
		echo "Published chart permalinks under $asset_branch/$run_path"
		echo "asset_base_url=https://raw.githubusercontent.com/${GITHUB_REPOSITORY}/${asset_commit}/${run_path}" >> "$GITHUB_OUTPUT"
		exit 0
	fi
	sleep "$attempt"
done

echo "Could not publish benchmark charts after five attempts." >&2
exit 1
