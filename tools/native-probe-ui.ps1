Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public static class HypeTabsNativeProbe {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left,top,right,bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    public static bool ForegroundChrome(uint pid) {
        var window=GetForegroundWindow();uint owner;GetWindowThreadProcessId(window,out owner);
        var cls=new StringBuilder(64);GetClassName(window,cls,64);
        return owner==pid && cls.ToString()=="Chrome_WidgetWin_1";
    }
    public static string WindowBounds(uint pid) {
        var prior=SetThreadDpiAwarenessContext((IntPtr)(-4));var rows=new StringBuilder();
        try {EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid && IsWindowVisible(h)){
            var cls=new StringBuilder(64);GetClassName(h,cls,64);Rect rect;
            if(cls.ToString()=="Chrome_WidgetWin_1" && GetWindowRect(h,out rect)) rows.AppendLine("Windows bounds: "+rect.left+","+rect.top+","+(rect.right-rect.left)+","+(rect.bottom-rect.top)+" DPI="+GetDpiForWindow(h));
        }return true;},IntPtr.Zero);return rows.ToString();}finally{SetThreadDpiAwarenessContext(prior);}
    }
    public delegate bool EnumProc(IntPtr h,IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p,IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint t,out IntPtr r);
    [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr h,uint m,IntPtr w,string text,uint f,uint t,out IntPtr r);
    [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr SendPointer(IntPtr h,uint m,IntPtr w,IntPtr l);
    public static IntPtr Find(uint pid) { IntPtr found=IntPtr.Zero;
        EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid){var c=new StringBuilder(64);GetClassName(h,c,64);if(c.ToString()=="HypeTabsSearch"){found=h;return false;}}return true;},IntPtr.Zero);return found;
    }
    public static bool CueVisible(uint pid) { bool found=false;
        EnumWindows((h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid && IsWindowVisible(h)){var c=new StringBuilder(64);GetClassName(h,c,64);if(c.ToString()=="HypeTabsCue"){found=true;return false;}}return true;},IntPtr.Zero);return found;
    }
    public static int Count(IntPtr list) {IntPtr value;return Send(list,0x018B,IntPtr.Zero,IntPtr.Zero,3,500,out value)!=IntPtr.Zero ? value.ToInt32() : -1;}
    public static bool Query(IntPtr input,string text) {IntPtr value;return SetText(input,0x000C,IntPtr.Zero,text,3,1000,out value)!=IntPtr.Zero && value!=IntPtr.Zero;}
    public static string Text(IntPtr control) {
        var buffer=Marshal.AllocHGlobal(4096*2);
        try { IntPtr copied; if(Send(control,0x000D,(IntPtr)4096,buffer,3,1000,out copied)==IntPtr.Zero) return null; return Marshal.PtrToStringUni(buffer,copied.ToInt32()); }
        finally {Marshal.FreeHGlobal(buffer);}
    }
    public static bool FirstRowClosed(IntPtr list) {
        IntPtr size;if(Send(list,0x018A,IntPtr.Zero,IntPtr.Zero,3,500,out size)==IntPtr.Zero || size.ToInt64()<0 || size.ToInt64()>20000) return false;
        var buffer=Marshal.AllocHGlobal((size.ToInt32()+1)*2);
        try { SendPointer(list,0x0189,IntPtr.Zero,buffer);return Marshal.PtrToStringUni(buffer).Contains(" / Closed "); }
        finally {Marshal.FreeHGlobal(buffer);}
    }
}
public sealed class HypeTabsTestSite : IDisposable {
    private readonly TcpListener listener=new TcpListener(IPAddress.Loopback,0);
    private readonly Thread worker;
    private volatile bool stopped;
    public int Port {get;private set;}
    public HypeTabsTestSite() {
        listener.Start();Port=((IPEndPoint)listener.LocalEndpoint).Port;
        worker=new Thread(Run);worker.IsBackground=true;worker.Start();
    }
    private void Run() {
        var body="<title>HypeTabs live restoration</title><p>Temporary local test page.</p>";
        var response=Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "+Encoding.ASCII.GetByteCount(body)+"\r\n\r\n"+body);
        while(!stopped) {
            try {using(var client=listener.AcceptTcpClient()) {client.ReceiveTimeout=1000;client.SendTimeout=1000;
                using(var stream=client.GetStream()) {var request=new byte[4096];int count=0;
                    while(count<request.Length){int read=stream.Read(request,count,request.Length-count);if(read==0)break;count+=read;if(Encoding.ASCII.GetString(request,0,count).Contains("\r\n\r\n"))break;}
                    stream.Write(response,0,response.Length);
                }
            }} catch(SocketException){if(stopped)break;} catch(System.IO.IOException){}
        }
    }
    public void Dispose(){stopped=true;listener.Stop();worker.Join(2500);}
}
'@
