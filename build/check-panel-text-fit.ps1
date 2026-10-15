<# Checks that the text of every visible label in a running PN-A2DP control panel fits its
   control, and that the longest text each Status value can show fits too. A plain label
   wraps, and a second line below a one-line control is not drawn, so the end of the text
   disappears; an end-ellipsis label shows "...". Read-only, except that -MinSize briefly
   shrinks the panel to its minimum size and then restores it.
   Run it with the panel open on the Status page of a PN-A2DP receiver, and again on its Headset
   page for the speaker-delay texts (a hidden page's controls are skipped). Exit 1 on any FAIL. #>
param([int]$ProcessId = 0, [switch]$MinSize, [switch]$ShowAll)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class PnFit {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int DrawText(IntPtr dc, string s, int n, ref RECT r, uint f);
  [DllImport("gdi32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr CreateFont(int h, int w, int e, int o, int wt, uint i, uint u, uint s, uint cs, uint op, uint cp, uint q, uint pf, string face);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr MainWindow(int pid) {
    IntPtr best = IntPtr.Zero; int area = 0;
    EnumWindows((h, l) => { int p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); int a = (r.R - r.L) * (r.B - r.T); if (a > area) { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best;
  }
  public static List<IntPtr> Children(IntPtr p) { var l = new List<IntPtr>(); EnumChildWindows(p, (h, x) => { l.Add(h); return true; }, IntPtr.Zero); return l; }
  public static string ClassName(IntPtr h) { var s = new StringBuilder(64); GetClassName(h, s, 64); return s.ToString(); }
  public static string Text(IntPtr h) { int n = (int)SendMessage(h, 0x000E, IntPtr.Zero, IntPtr.Zero); var s = new StringBuilder(n + 2); SendMessage(h, 0x000D, (IntPtr)(n + 1), s); return s.ToString(); }
  /* The size DrawText needs at the control's width: { widest line, height }. */
  public static int[] Need(IntPtr dc, string t, int width, bool wrap, bool noPrefix) {
    RECT r = new RECT(); r.R = width;
    uint f = 0x400 /* CALCRECT */ | 0x40 /* EXPANDTABS */ | (wrap ? 0x10u /* WORDBREAK */ : 0x20u /* SINGLELINE */) | (noPrefix ? 0x800u : 0);
    DrawText(dc, t, -1, ref r, f); return new int[] { r.R - r.L, r.B - r.T };
  }
}
'@

# The longest text each Status value and each Headset page speaker-delay text can show (panel.cpp
# LoadStatus and the speaker delay functions, tools/common/pnctl.cpp, tools/common/pnlatency.cpp).
# Keep this table in step with those strings. Reports are at most 6553.5 ms; an ignored one is
# below 80 ms; a fixed delay is at most 1000 ms.
$longest = @{
    1103 = @('served by the Microsoft inbox driver', 'PN-A2DP driver not responding')                 # State
    1104 = @('aptX Low Latency (ABR reduced)')                                                         # Codec
    1105 = @('44100 Hz, 2 ch, 32-bit in', '48000 Hz, 2 ch, 32-bit in')                                # Format
    1115 = @('48000 Hz, converted to 44100 Hz', '88200 Hz, converted to 44100 Hz', '96000 Hz, converted to 48000 Hz', '44100 Hz, no conversion')  # Windows rate
    1106 = @('codec host plugin')                                                                      # Encoder
    1107 = @('1000 kbps (ceiling 1000 kbps)')                                                          # Bitrate
    1108 = @('1500 ms estimated end to end')                                                           # Latency
    1109 = @('not reported; a per-codec default is assumed', '6553.5 ms reported by the headset',      # Sink delay (drivers before 0.1.0.25)
             '6553.5 ms reported by the speaker', '1000 ms fixed; the speaker reports 6553.5 ms',        # Sink delay (pnctl SinkDelayShortText)
             '1000 ms fixed; the 79.9 ms report is ignored', '180 ms default; the 79.9 ms report is ignored')
    1113 = @('audio running, in range, rate converted (2:1)')                                         # Flags
    1127 = @('64 now, 64 peak')                                                                        # TX queue
    1128 = @('65535 us avg, 999999 us max')                                                            # TX completion
    1129 = @('level 10, 100 down, 100 up')                                                             # ABR
    1140 = @('not installed (only SBC is available; run the installer or Driver > Install)')           # Codec host service
    1141 = @('kernel: SBC   codec host: SBC AAC aptX aptX HD aptX Low Latency LDAC')                  # Encoders
    1142 = @('PN-A2DP 0.1.0.65535 (ABI 1)')                                                            # Driver
    # Headset page, speaker delay (lip-sync): status line (pnctl SinkDelayText), the fixed delay's codec
    1805 = @('Using 1000 ms, the fixed speaker delay; the speaker''s report of 79.9 ms is ignored as impossible',
             'Using 180 ms, the aptX HD default; the speaker''s report of 79.9 ms is ignored as impossible',
             'Using 60 ms, the aptX Low Latency default; the speaker does not report its delay',
             'Speaker delay settings and the measurement need driver 0.1.0.25 or later.')
    1809 = @('ms, for aptX Low Latency')
    # Headset page: the measurement's hint, progress and results (two lines; panel.cpp, pnlatency.cpp)
    1812 = @('Measure with microphone: the speaker plays 8 short chirps, the microphone hears when they come out, and the speaker delay it finds is entered as the fixed delay for the current codec. Apply changes saves it.',
             'No microphone is available. Connect one (a USB microphone works well), allow desktop apps to use it (Windows Settings > Privacy & security > Microphone), then press Refresh.',
             'The microphone recorded only silence. Check that it is not muted and that desktop apps may use it (Windows Settings > Privacy & security > Microphone), then try again.',
             'The chirps arrived with delays that differ by 1999 ms. Background noise, or the speaker changed its delay during the test. Try again in a quiet room with no other sound playing.',
             'This microphone records at 11025 Hz, too low for the test sound (Bluetooth headset microphones often record this way). Choose a microphone that records at 16 kHz or more.',
             'The speaker changed its delay during the test (the driver applied 6553.5 ms, then 1000 ms). Play nothing else to it, wait 10 seconds, then try again.',
             'The driver did not report the speaker delay it applies, so the result cannot be turned into a setting. Driver 0.1.0.25 or later is needed.',
             'The speaker''s audio clock did not start. Check that the speaker is connected and not used exclusively by another app, then try again.',
             'The output''s audio clock stopped during the test (7 of 8 chirps timed). Check that the speaker stays connected, then try again.',
             'Measured 1000 ms (8 of 8 chirps, spread 20 ms). Apply changes to use it.')
}

if (-not $ProcessId) {
    $p = Get-Process pna2dp-panel -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if (-not $p) { throw 'The PN-A2DP control panel is not running.' }
    $ProcessId = $p.Id
}
$main = [PnFit]::MainWindow($ProcessId)
if ($main -eq [IntPtr]::Zero) { throw "No visible panel window for process $ProcessId." }

$restore = $null
if ($MinSize -and -not [PnFit]::IsZoomed($main)) {
    $restore = New-Object PnFit+RECT; [void][PnFit]::GetWindowRect($main, [ref]$restore)
    [void][PnFit]::SetWindowPos($main, [IntPtr]::Zero, 0, 0, 1, 1, 0x16)   # NOMOVE | NOZORDER | NOACTIVATE; clamps to the minimum
    Start-Sleep -Milliseconds 400
}

$dpi = [PnFit]::GetDpiForWindow($main)
$dc = [PnFit]::GetDC([IntPtr]::Zero)
# Every page's dialog template font: Segoe UI 9 pt at the window's DPI.
$font = [PnFit]::CreateFont(-[int][math]::Round(9 * $dpi / 72), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, 'Segoe UI')
$oldFont = [PnFit]::SelectObject($dc, $font)
$fail = 0; $checked = 0
try {
    foreach ($h in [PnFit]::Children($main)) {
        if ([PnFit]::ClassName($h) -ne 'Static' -or -not [PnFit]::IsWindowVisible($h)) { continue }
        $style = [PnFit]::GetWindowLong($h, -16)
        $type = $style -band 0x1F
        if ($type -notin 0x0, 0xC) { continue }                    # SS_LEFT and SS_LEFTNOWORDWRAP text only
        $r = New-Object PnFit+RECT; [void][PnFit]::GetClientRect($h, [ref]$r)
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        if ($w -le 0 -or $ht -le 0) { continue }
        $id = [PnFit]::GetDlgCtrlID($h)
        $wrap = ($type -eq 0) -and -not ($style -band 0x4000)      # SS_ENDELLIPSIS draws a single line
        foreach ($t in @([PnFit]::Text($h)) + @($longest[$id] | Where-Object { $_ })) {
            if (-not $t) { continue }
            $checked++
            $need = [PnFit]::Need($dc, $t, $w, $wrap, [bool]($style -band 0x80))
            $cut = if ($wrap) { $need[1] -gt $ht } else { $need[0] -gt $w }
            if ($cut) { $fail++; Write-Host "FAIL control $id (${w}x${ht} px) needs $($need[0])x$($need[1]) px: '$t'" }
            elseif ($ShowAll) { Write-Host "ok   control $id (${w}x${ht} px) needs $($need[0])x$($need[1]) px: '$t'" }
        }
    }
} finally {
    [void][PnFit]::SelectObject($dc, $oldFont); [void][PnFit]::DeleteObject($font); [void][PnFit]::ReleaseDC([IntPtr]::Zero, $dc)
    if ($restore) { [void][PnFit]::SetWindowPos($main, [IntPtr]::Zero, 0, 0, $restore.R - $restore.L, $restore.B - $restore.T, 0x16) }
}
Write-Host "$checked texts checked at $dpi DPI$(if ($MinSize) { ', minimum window size' }); $fail cut off"
if ($fail) { exit 1 }
