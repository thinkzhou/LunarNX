---
name: send-feishu-build
description: Send LunarNX Nintendo Switch NRO builds through lark-cli, or receive/search LunarNX-related Feishu messages and download their attachments. Use when the user asks to send, push, deliver, upload, find, read, receive, or download LunarNX builds, logs, screenshots, or test feedback in Feishu, including bounded real-time message listening.
---

# LunarNX Feishu Messages

Use the authenticated `lark-cli` profile for LunarNX Feishu messaging. Use the
installed `lark-shared` skill before authentication or permission recovery,
`lark-im` before message operations, and `lark-event` before real-time
listening.

## Send an NRO

1. Treat sending as an external action. Run the script only when the user explicitly asks to send the file.
2. For the normal application, use `build/switch/LunarNX.nro`. Never select another diagnostic or probe NRO merely because it is newer.
3. If the user asks for the latest build after source changes, first build the Switch NRO in Docker and satisfy the repository's `AGENTS.md` verification requirements.
4. For a diagnostic, smoke, replay, or probe build, require or infer an unambiguous path from the user's request and pass that path explicitly.
5. Summarize the user-visible changes in concise Chinese from the actual commits or diff included in the build. Use one to three bullet points and pass them with the required `--change-summary`. Do not use raw commit hashes, file lists, or generic text such as "latest build" as the summary.
6. Run from any directory:

   ```sh
   .agents/skills/send-feishu-build/scripts/send_nro.sh \
     --change-summary $'- 修复长时间串流时统计计数回绕的问题\n- 改进传输诊断信息' \
     [path/to/build.nro]
   ```

   Omitting the path sends `build/switch/LunarNX.nro`.
   Use `--dry-run` before the first real send or when validating a newly built artifact:

   ```sh
   .agents/skills/send-feishu-build/scripts/send_nro.sh --dry-run [path/to/build.nro]
   ```
7. Send as `bot`. Confirm the target group, release-note content, and bot identity before the external action. The configured GPTBot must be a member of the target group.
8. Report the displayed filename, byte size, SHA-256, Git commit, and CLI result locally. Do not include those diagnostics in the Feishu message, and do not print credentials, access tokens, or the group ID.

## Receive messages and files

Prefer on-demand reads over a permanent listener.

1. Use user identity for searches and downloads unless the user explicitly requests bot identity:

   ```sh
   lark-cli im +messages-search --as user --query "<keyword>" \
     --chat-id <chat_id> --page-all --format json
   ```

2. If the user names a group but no `chat_id`, resolve it first with `lark-cli im +chat-search --as user --query "<group name>" --format json`. Use `feishu-chat-id` from macOS Keychain only when the user means the configured LunarNX delivery group.
3. For a known attachment, keep the `message_id` and matching `file_key` from the same message, then download into a user-approved project-relative inbox:

   ```sh
   lark-cli im +messages-resources-download --as user \
     --message-id <om_xxx> --file-key <file_xxx> --type file \
     --output ./<inbox>/<filename>
   ```

4. Never download into the repository by default. Use a temporary directory unless the user asks to keep the file. Never commit downloaded files, credentials, tokens, logs, simulator data, or build artifacts.
5. Preserve sender, group, timestamp, and `message_id` when reporting received content so the user can identify the source.

## Listen for new messages

Only start a listener when the user explicitly asks to wait, monitor, or receive new messages. Inspect the event schema first, then use a bounded run:

```sh
lark-cli event schema im.message.receive_v1 --json
lark-cli event consume im.message.receive_v1 --as bot \
  --max-events 1 --timeout 10m
```

Do not silently create a permanent background service. For longer monitoring, use a user-approved automation or service and preserve the event consumer's ready marker and graceful shutdown contract.

## Message template

Edit `assets/message-template.txt` to change the Feishu release-note format. Preserve real line breaks. Put `{{CHANGE_SUMMARY}}` on its own line where the generated bullet list should appear. `{{FILE_NAME}}` remains available when a filename is genuinely useful, but the default template omits it because Feishu already displays the attachment name.

## Configuration

Require an authenticated `lark-cli` profile and the macOS generic-password item `feishu-chat-id` for the default delivery group. App credentials and access tokens belong to `lark-cli`; do not read or duplicate them in this skill.

## Safety

- Accept only regular `.nro` files smaller than Feishu's 30,000,000-byte message-file limit.
- Do not send ZIP files unless the user needs a full SD-card directory layout or accompanying resources.
- Never commit credentials, tokens, auth responses, generated logs, NROs, or other build artifacts.
- Do not automatically retry a successful upload when the message result is uncertain; report the failure to avoid duplicate messages.
- Treat sending as an external write: require explicit user approval of recipient, content, and identity.
