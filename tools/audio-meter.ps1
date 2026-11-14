# Reports whether Windows' default output is muted, its volume, and which programs are sending
# sound to it right now (their peak level over a short listening period).
#   .\tools\audio-meter.ps1 [-Seconds 3]
# Useful when a plug-in "makes no sound": it separates "nothing is being played" from "it is
# being played but the output is muted or turned down".
param([double]$Seconds = 3)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace AudioMeter {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    }

    [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioEndpointVolume {
        int RegisterControlChangeNotify(IntPtr notify);
        int UnregisterControlChangeNotify(IntPtr notify);
        int GetChannelCount(out int count);
        int SetMasterVolumeLevel(float level, ref Guid context);
        int SetMasterVolumeLevelScalar(float level, ref Guid context);
        int GetMasterVolumeLevel(out float level);
        int GetMasterVolumeLevelScalar(out float level);
        int SetChannelVolumeLevel(int channel, float level, ref Guid context);
        int SetChannelVolumeLevelScalar(int channel, float level, ref Guid context);
        int GetChannelVolumeLevel(int channel, out float level);
        int GetChannelVolumeLevelScalar(int channel, out float level);
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid context);
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
    }

    [ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioMeterInformation {
        int GetPeakValue(out float peak);
    }

    [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionManager2 {
        int GetAudioSessionControl(IntPtr sessionGuid, int flags, out IntPtr control);
        int GetSimpleAudioVolume(IntPtr sessionGuid, int flags, out IntPtr volume);
        int GetSessionEnumerator(out IAudioSessionEnumerator enumerator);
    }

    [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionEnumerator {
        int GetCount(out int count);
        int GetSession(int index, [MarshalAs(UnmanagedType.IUnknown)] out object session);
    }

    [ComImport, Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionControl2 {
        int GetState(out int state);
        int GetDisplayName(out IntPtr name);
        int SetDisplayName(string name, ref Guid context);
        int GetIconPath(out IntPtr path);
        int SetIconPath(string path, ref Guid context);
        int GetGroupingParam(out Guid param);
        int SetGroupingParam(ref Guid param, ref Guid context);
        int RegisterAudioSessionNotification(IntPtr notify);
        int UnregisterAudioSessionNotification(IntPtr notify);
        int GetSessionIdentifier(out IntPtr id);
        int GetSessionInstanceIdentifier(out IntPtr id);
        int GetProcessId(out uint pid);
    }

    public class Session { public uint Pid; public float Peak; public int State; }

    public static class Meter {
        public static bool Muted; public static float Volume; public static float OutputPeak;

        public static List<Session> Listen(double seconds) {
            var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            IMMDevice dev; en.GetDefaultAudioEndpoint(0, 1, out dev);
            object o;
            Guid iidVol = typeof(IAudioEndpointVolume).GUID; dev.Activate(ref iidVol, 23, IntPtr.Zero, out o);
            var vol = (IAudioEndpointVolume)o; vol.GetMute(out Muted); vol.GetMasterVolumeLevelScalar(out Volume);
            Guid iidMeter = typeof(IAudioMeterInformation).GUID; dev.Activate(ref iidMeter, 23, IntPtr.Zero, out o);
            var outMeter = (IAudioMeterInformation)o;
            Guid iidMgr = typeof(IAudioSessionManager2).GUID; dev.Activate(ref iidMgr, 23, IntPtr.Zero, out o);
            var mgr = (IAudioSessionManager2)o;
            IAudioSessionEnumerator se; mgr.GetSessionEnumerator(out se);
            int n; se.GetCount(out n);
            var list = new List<Session>(); var meters = new List<IAudioMeterInformation>();
            for (int i = 0; i < n; i++) {
                object s; se.GetSession(i, out s);
                var c = (IAudioSessionControl2)s; var s2 = new Session();
                c.GetProcessId(out s2.Pid); c.GetState(out s2.State);
                list.Add(s2); meters.Add((IAudioMeterInformation)s);
            }
            OutputPeak = 0;
            var end = DateTime.UtcNow.AddSeconds(seconds);
            while (DateTime.UtcNow < end) {
                float p; outMeter.GetPeakValue(out p); if (p > OutputPeak) OutputPeak = p;
                for (int i = 0; i < list.Count; i++) { meters[i].GetPeakValue(out p); if (p > list[i].Peak) list[i].Peak = p; }
                System.Threading.Thread.Sleep(20);
            }
            return list;
        }
    }
}
'@
$sessions = [AudioMeter.Meter]::Listen($Seconds)
"Windows default output: {0}, volume {1:P0}; loudest level on the output in {2} s: {3:N3}" -f $(if ([AudioMeter.Meter]::Muted) { 'MUTED' } else { 'not muted' }), [AudioMeter.Meter]::Volume, $Seconds, [AudioMeter.Meter]::OutputPeak
foreach ($s in $sessions | Sort-Object Peak -Descending) {
    $name = if ($s.Pid -eq 0) { 'System sounds' } else { try { (Get-Process -Id $s.Pid -ErrorAction Stop).ProcessName } catch { "pid $($s.Pid)" } }
    "  {0,-28} {1}   loudest level {2:N3}" -f $name, $(if ($s.State -eq 1) { 'playing ' } else { 'inactive' }), $s.Peak
}
