// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Note: This is a GitHub Actions script
// It is not meant to be executed directly on your machine without modifications

const fs = require("fs");
// which label to check for changes
const CHANGE_LABEL_MAINLINE = 'android-merge';
const CHANGE_LABEL_EA = 'android-ea-merge';
// how far back in time should we consider the changes are "recent"? (default: 24 hours)
const DETECTION_TIME_FRAME = (parseInt(process.env.DETECTION_TIME_FRAME)) || (24 * 3600 * 1000);
const BUILD_EA = process.env.BUILD_EA == 'true';
const MAINLINE_TAG = process.env.MAINLINE_TAG;

async function checkBaseChanges(github) {
    // query the commit date of the latest commit on this branch
    const query = `query($owner:String!, $name:String!, $ref:String!) {
        repository(name:$name, owner:$owner) {
            ref(qualifiedName:$ref) {
                target {
                    ... on Commit { id pushedDate oid }
                }
            }
        }
    }`;
    const variables = {
        owner: 'yuzu-emu',
        name: 'yuzu',
        ref: 'refs/heads/master',
    };
    const result = await github.graphql(query, variables);
    const pushedAt = result.repository.ref.target.pushedDate;
    console.log(`Last commit pushed at ${pushedAt}.`);
    const delta = new Date() - new Date(pushedAt);
    if (delta <= DETECTION_TIME_FRAME) {
        console.info('New changes detected, triggering a new build.');
        return true;
    }
    console.info('No new changes detected.');
    return false;
}

async function checkAndroidChanges(github) {
    if (checkBaseChanges(github)) return true;
    const pulls = getPulls(github, false);
    for (let i = 0; i < pulls.length; i++) {
        let pull = pulls[i];
        if (new Date() - new Date(pull.headRepository.pushedAt) <= DETECTION_TIME_FRAME) {
            console.info(`${pull.number} updated at ${pull.headRepository.pushedAt}`);
            return true;
        }
    }
    console.info("No changes detected in any tagged pull requests.");
    return false;
}

async function tagAndPush(github, owner, repo, execa, commit=false) {
    let altToken = process.env.ALT_GITHUB_TOKEN;
    if (!altToken) {
        throw `Please set ALT_GITHUB_TOKEN environment variable. This token should have write access to ${owner}/${repo}.`;
    }
    const query = `query ($owner:String!, $name:String!) {
        repository(name:$name, owner:$owner) {
            refs(refPrefix: "refs/tags/", orderBy: {field: TAG_COMMIT_DATE, direction: DESC}, first: 10) {
                nodes { name }
            }
        }
    }`;
    const variables = {
        owner: owner,
        name: repo,
    };
    const tags = await github.graphql(query, variables);
    const tagList = tags.repository.refs.nodes;
    let lastTag = 'android-1';
    for (let i = 0; i < tagList.length; ++i) {
        if (tagList[i].name.includes('android-')) {
            lastTag = tagList[i].name;
            break;
        }
    }
    const tagNumber = /\w+-(\d+)/.exec(lastTag)[1] | 0;
    const channel = repo.split('-')[1];
    const newTag = `${channel}-${tagNumber + 1}`;
    console.log(`New tag: ${newTag}`);
    if (commit) {
        let channelName = channel[0].toUpperCase() + channel.slice(1);
        console.info(`Committing pending commit as ${channelName} ${tagNumber + 1}`);
        await execa("git", ['commit', '-m', `${channelName} ${tagNumber + 1}`]);
    }
    console.info('Pushing tags to GitHub ...');
    await execa("git", ['tag', newTag]);
    await execa("git", ['remote', 'add', 'target', `https://${altToken}@github.com/${owner}/${repo}.git`]);
    await execa("git", ['push', 'target', 'master', '-f']);
    await execa("git", ['push', 'target', 'master', '--tags']);
    console.info('Successfully pushed new changes.');
}

async function tagAndPushEA(github, owner, repo, execa) {
    let altToken = process.env.ALT_GITHUB_TOKEN;
    if (!altToken) {
        throw `Please set ALT_GITHUB_TOKEN environment variable. This token should have write access to ${owner}/${repo}.`;
    }
    const query = `query ($owner:String!, $name:String!) {
        repository(name:$name, owner:$owner) {
            refs(refPrefix: "refs/tags/", orderBy: {field: TAG_COMMIT_DATE, direction: DESC}, first: 10) {
                nodes { name }
            }
        }
    }`;
    const variables = {
        owner: owner,
        name: repo,
    };
    const tags = await github.graphql(query, variables);
    const tagList = tags.repository.refs.nodes;
    let lastTag = 'ea-1';
    for (let i = 0; i < tagList.length; ++i) {
        if (tagList[i].name.includes('ea-')) {
            lastTag = tagList[i].name;
            break;
        }
    }
    const tagNumber = /\w+-(\d+)/.exec(lastTag)[1] | 0;
    const newTag = `ea-${tagNumber + 1}`;
    console.log(`New tag: ${newTag}`);
    console.info('Pushing tags to GitHub ...');
    await execa("git", ["remote", "add", "android", "https://github.com/yuzu-mirror/yuzu-android.git"]);
    await execa("git", ["fetch", "android"]);

    await execa("git", ['tag', newTag]);
    await execa("git", ['push', 'android', `${newTag}`]);

    fs.writeFile('tag-name.txt', newTag, (err) => {
        if (err) throw 'Could not write tag name to file!'
    })

    console.info('Successfully pushed new changes.');
}

async function generateReadme(pulls, context, mergeResults, execa) {
    let baseUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/`;
    let output =
        "| Pull Request | Commit | Title | Author | Merged? |\n|----|----|----|----|----|\n";
    for (let pull of pulls) {
        let pr = pull.number;
        let result = mergeResults[pr];
        output += `| [${pr}](${baseUrl}/pull/${pr}) | [\`${result.rev || "N/A"}\`](${baseUrl}/pull/${pr}/files) | ${pull.title} | [${pull.author.login}](https://github.com/${pull.author.login}/) | ${result.success ? "Yes" : "No"} |\n`;
    }
    output +=
        "\n\nEnd of merge log. You can find the original README.md below the break.\n\n-----\n\n";
    output += fs.readFileSync("./README.md");
    fs.writeFileSync("./README.md", output);
    await execa("git", ["add", "README.md"]);
}

async function fetchPullRequests(pulls, repoUrl, execa) {
    console.log("::group::Fetch pull requests");
    for (let pull of pulls) {
        let pr = pull.number;
        console.info(`Fetching PR ${pr} ...`);
        await execa("git", [
            "fetch",
            "-f",
            "--no-recurse-submodules",
            repoUrl,
            `pull/${pr}/head:pr-${pr}`,
        ]);
    }
    console.log("::endgroup::");
}

async function mergePullRequests(pulls, execa) {
    let mergeResults = {};
    console.log("::group::Merge pull requests");
    await execa("git", ["config", "--global", "user.name", "yuzubot"]);
    await execa("git", [
        "config",
        "--global",
        "user.email",
        "yuzu\x40yuzu-emu\x2eorg", // prevent email harvesters from scraping the address
    ]);
    let hasFailed = false;
    for (let pull of pulls) {
        let pr = pull.number;
        console.info(`Merging PR ${pr} ...`);
        try {
            const process1 = execa("git", [
                "merge",
                "--squash",
                "--no-edit",
                `pr-${pr}`,
            ]);
            process1.stdout.pipe(process.stdout);
            await process1;

            const process2 = execa("git", ["commit", "-m", `Merge yuzu-emu#${pr}`]);
            process2.stdout.pipe(process.stdout);
            await process2;

            const process3 = await execa("git", ["rev-parse", "--short", `pr-${pr}`]);
            mergeResults[pr] = {
                success: true,
                rev: process3.stdout,
            };
        } catch (err) {
            console.log(
                `::error title=#${pr} not merged::Failed to merge pull request: ${pr}: ${err}`
            );
            mergeResults[pr] = { success: false };
            hasFailed = true;
            await execa("git", ["reset", "--hard"]);
        }
    }
    console.log("::endgroup::");
    if (hasFailed) {
        throw 'There are merge failures. Aborting!';
    }
    return mergeResults;
}

async function resetBranch(execa) {
    console.log("::group::Reset master branch");
    let hasFailed = false;
    try {
        await execa("git", ["remote", "add", "source", "https://github.com/yuzu-mirror/yuzu.git"]);
        await execa("git", ["fetch", "source"]);
        const process1 = await execa("git", ["rev-parse", "source/master"]);
        const headCommit = process1.stdout;

        await execa("git", ["reset", "--hard", headCommit]);
    } catch (err) {
        console.log(`::error title=Failed to reset master branch`);
        hasFailed = true;
    }
    console.log("::endgroup::");
    if (hasFailed) {
        throw 'Failed to reset the master branch. Aborting!';
    }
}

async function getPulls(github) {
    const query = `query ($owner:String!, $name:String!, $label:String!) {
        repository(name:$name, owner:$owner) {
            pullRequests(labels: [$label], states: OPEN, first: 100) {
                nodes {
                    number title author { login }
                }
            }
        }
    }`;
    const mainlineVariables = {
        owner: 'yuzu-emu',
        name: 'yuzu',
        label: CHANGE_LABEL_MAINLINE,
    };
    const mainlineResult = await github.graphql(query, mainlineVariables);
    const pulls = mainlineResult.repository.pullRequests.nodes;
    if (BUILD_EA) {
        const eaVariables = {
            owner: 'yuzu-emu',
            name: 'yuzu',
            label: CHANGE_LABEL_EA,
        };
        const eaResult = await github.graphql(query, eaVariables);
        const eaPulls = eaResult.repository.pullRequests.nodes;
        return pulls.concat(eaPulls);
    }
    return pulls;
}

async function getMainlineTag(execa) {
    console.log(`::group::Getting mainline tag android-${MAINLINE_TAG}`);
    let hasFailed = false;
    try {
        await execa("git", ["remote", "add", "mainline", "https://github.com/yuzu-mirror/yuzu-android.git"]);
        await execa("git", ["fetch", "mainline", "--tags"]);
        await execa("git", ["checkout", `tags/android-${MAINLINE_TAG}`]);
        await execa("git", ["submodule", "update", "--init", "--recursive"]);
    } catch (err) {
        console.log('::error title=Failed pull tag');
        hasFailed = true;
    }
    console.log("::endgroup::");
    if (hasFailed) {
        throw 'Failed pull mainline tag. Aborting!';
    }
}

async function mergebot(github, context, execa) {
    // Reset our local copy of master to what appears on yuzu-emu/yuzu - master
    await resetBranch(execa);

    const pulls = await getPulls(github);
    let displayList = [];
    for (let i = 0; i < pulls.length; i++) {
        let pull = pulls[i];
        displayList.push({ PR: pull.number, Title: pull.title });
    }
    console.info("The following pull requests will be merged:");
    console.table(displayList);
    await fetchPullRequests(pulls, "https://github.com/yuzu-mirror/yuzu", execa);
    const mergeResults = await mergePullRequests(pulls, execa);

    if (BUILD_EA) {
        await tagAndPushEA(github, 'yuzu-emu', `yuzu-android`, execa);
    } else {
        await generateReadme(pulls, context, mergeResults, execa);
        await tagAndPush(github, 'yuzu-emu', `yuzu-android`, execa, true);
    }
}

module.exports.mergebot = mergebot;
module.exports.checkAndroidChanges = checkAndroidChanges;
module.exports.tagAndPush = tagAndPush;
module.exports.checkBaseChanges = checkBaseChanges;
module.exports.getMainlineTag = getMainlineTag;
