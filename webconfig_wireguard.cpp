#include "webconfig_wireguard.h"

String webconfigWireGuardWifiSectionHtml()
{
    String html;

    html +=
        "<style>"
        ".vpn-card{border:1px solid var(--border);border-radius:14px;overflow:hidden;background:var(--card)}"
        ".vpn-card-head{display:flex;align-items:center;gap:14px;padding:16px 18px;border-bottom:1px solid var(--border)}"
        ".vpn-card-icon{display:flex;align-items:center;justify-content:center;width:42px;height:42px;min-width:42px;border-radius:12px;background:#eef2f7;color:#334155}"
        ".vpn-card-icon svg{width:23px;height:23px;display:block}"
        ".vpn-card-title{min-width:0;flex:1}.vpn-card-title strong{display:block;font-size:1rem}.vpn-card-title span{display:block;margin-top:2px;color:var(--muted);font-size:.86rem}"
        ".vpn-status-pill{display:inline-flex;align-items:center;gap:7px;padding:6px 10px;border-radius:999px;border:1px solid #d1d5db;background:#f8fafc;color:#475569;font-size:.82rem;font-weight:700;white-space:nowrap}"
        ".vpn-status-dot{width:8px;height:8px;border-radius:50%;background:#94a3b8}"
        ".vpn-card-body{padding:16px 18px;line-height:1.5}"
        ".vpn-card-body p{margin:0}.vpn-card-body .vpn-primary{font-weight:600;color:var(--text)}"
        "@media(max-width:640px){.vpn-card-head{align-items:flex-start;flex-wrap:wrap}.vpn-status-pill{margin-left:56px}.vpn-card-body{padding:14px 16px}}"
        "</style>";

    html +=
        "<section id='vpn' class='settings-section'>"
        "<h3>VPN / WireGuard</h3>"
        "<div class='vpn-card'>"
        "<div class='vpn-card-head'>"
        "<div class='vpn-card-icon' aria-hidden='true'>"
        "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>"
        "<path d='M12 3l7 3v5c0 4.8-2.8 8.2-7 10-4.2-1.8-7-5.2-7-10V6l7-3z'/><path d='M9.2 12l1.8 1.8 3.8-4'/>"
        "</svg></div>"
        "<div class='vpn-card-title'><strong>Sicherer VPN-Zugriff</strong><span>WireGuard</span></div>"
        "<div class='vpn-status-pill'><span class='vpn-status-dot'></span>Derzeit nicht verfügbar</div>"
        "</div>"
        "<div class='vpn-card-body'>"
        "<p class='vpn-primary'>WireGuard/VPN ist auf diesem Board derzeit nicht verfügbar.</p>"
        "</div></div></section>";

    return html;
}
