#!/usr/bin/env python3
"""Exercise actual compiled bridge lookup/publication against isolated JVM capabilities."""
import glob
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLASSES = ROOT / 'app/build/intermediates/built_in_kotlinc/debug/compileDebugKotlin/classes'
STDLIB = glob.glob(os.path.expanduser('~/.gradle/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/*/*/kotlin-stdlib-*.jar'))[-1]
API = glob.glob(os.path.expanduser('~/.gradle/caches/*/transforms/*/transformed/api-102.0.0/jars/classes.jar'))[0]
SOURCES = {
'android/graphics/Rect.java': '''package android.graphics; public class Rect { public int left,top,right,bottom; public Rect(int l,int t,int r,int b){left=l;top=t;right=r;bottom=b;} public Rect(Rect r){this(r.left,r.top,r.right,r.bottom);} public int width(){return right-left;} public int height(){return bottom-top;} public boolean equals(Object o){return o instanceof Rect r && left==r.left && top==r.top && right==r.right && bottom==r.bottom;} public int hashCode(){return left+top+right+bottom;} }''',
'android/graphics/Point.java': 'package android.graphics; public record Point(int x,int y) {}',
'android/os/Parcelable.java': 'package android.os; public interface Parcelable {interface Creator<T>{T createFromParcel(Parcel p);}}',
'android/os/Parcel.java': '''package android.os; import java.util.*; public class Parcel {public final List<Integer> values=new ArrayList<>(); public static Parcel obtain(){return new Parcel();}public void writeNoException(){}public void writeInt(int i){values.add(i);}public void appendFrom(Parcel p,int o,int s){} public int dataSize(){return 0;}public void recycle(){} }''',
'android/os/Looper.java': 'package android.os; public class Looper {}',
'android/os/HandlerThread.java': 'package android.os; public class HandlerThread {public HandlerThread(String s){}public void start(){}public Looper getLooper(){return new Looper();}public boolean quitSafely(){return true;}}',
'android/os/Handler.java': '''package android.os; import java.util.*; public class Handler {public static final List<Runnable> delayed=new ArrayList<>();public Handler(Looper l){}public boolean post(Runnable r){return true;}public boolean postDelayed(Runnable r,long t){delayed.add(r);return true;}public void removeCallbacksAndMessages(Object o){} }''',
'android/content/Context.java': 'package android.content; public class Context {public ContentResolver getContentResolver(){return new ContentResolver();}}',
'android/content/ContentResolver.java': 'package android.content; public class ContentResolver {}',
'android/content/pm/ApplicationInfo.java': 'package android.content.pm; public class ApplicationInfo {public int uid=10042;}',
'android/content/pm/ActivityInfo.java': 'package android.content.pm; public class ActivityInfo {public ApplicationInfo applicationInfo=new ApplicationInfo();}',
'android/provider/Settings.java': 'package android.provider; public class Settings {public static class Global {public static final String ANIMATOR_DURATION_SCALE="a",WINDOW_ANIMATION_SCALE="w",TRANSITION_ANIMATION_SCALE="t";public static float getFloat(android.content.ContentResolver r,String k,float d){return d;}}}',
'android/util/Log.java': 'package android.util; public class Log {public static final int DEBUG=3,INFO=4,WARN=5,ERROR=6;}',
'android/view/WindowManager.java': '''package android.view; public interface WindowManager {class LayoutParams {public static final int FLAG_SECURE=8192;public int type=3,flags=0;public String packageName="test.app";}}''',
'android/view/SurfaceControl.java': '''package android.view; import android.os.*; public class SurfaceControl {public Object mLock=new Object();public long mNativeObject;public int id,mWidth=1120,mHeight=2436;public boolean valid=true;public static int applies;public SurfaceControl(){}public SurfaceControl(int i){id=i;mNativeObject=i;}public SurfaceControl(SurfaceControl s,String name){id=s.id;mNativeObject=s.mNativeObject;mWidth=s.mWidth;mHeight=s.mHeight;}public boolean isValid(){return valid;}public int getLayerId(){return id;}public String getName(){return "capability";}public int getWidth(){return mWidth;}public int getHeight(){return mHeight;}public void release(){valid=false;}public static Parcelable.Creator<Object> CREATOR=p->null;private static void nativeWriteToParcel(long p,Parcel out){}private static void nativeWriteTransactionToParcel(long p,Parcel out){}public static class Transaction {public long mNativeObject=500;public Transaction(){}public void apply(){applies++;}public void close(){}}}''',
'io/github/libxposed/api/XposedModule.java': 'package io.github.libxposed.api; public class XposedModule {public void log(int p,String tag,String text){}public void log(int p,String tag,String text,Throwable t){}}',
'com/android/server/wm/WindowManagerService.java': 'package com.android.server.wm; public class WindowManagerService {}',
'com/android/server/wm/PreStartingManager.java': '''package com.android.server.wm; import java.util.*; import java.util.concurrent.locks.*; public class PreStartingManager {public static final PreStartingManager INSTANCE=new PreStartingManager();public static PreStartingManager getInstance(){return INSTANCE;}public final ReentrantLock mSurfaceLock=new ReentrantLock();public final Map<Object,Object> mPreSurfaceControlMap=new HashMap<>(),mColorSurfaceControlMap=new HashMap<>(),mPreSurfaceInitPosMap=new HashMap<>(),mPreSurfaceScaleMap=new HashMap<>(),mPendingRemoveStartingData=new HashMap<>();}''',
'ng/lst/genie/ContentSurfaces.java': '''package ng.lst.genie; import android.view.*; public class ContentSurfaces {public static final ContentSurfaces INSTANCE=new ContentSurfaces();public static int replacement=-1;public Object copy(Object s,String name,int w,int h){var r=new SurfaceControl((SurfaceControl)s,name);if(replacement>=0){r.id=replacement;r.mNativeObject=replacement;}return r;}}''',
'ng/lst/genie/ContentAnimator.java': '''package ng.lst.genie; public class ContentAnimator {public static final ContentAnimator INSTANCE=new ContentAnimator();public static int primes;public static long[] companions;public static long surface,parent;public boolean prime(long t,long s,long p,int w,int h,float[] g,boolean closing,long epoch,long[] c,boolean starting,long unwarped){if(!closing||!starting||unwarped!=0||t!=500)throw new AssertionError("preview publication contract");primes++;surface=s;parent=p;companions=c;return true;}public static boolean prime$default(ContentAnimator a,long t,long s,long p,int w,int h,float[] g,boolean close,long epoch,long[] c,boolean starting,long unwarped,int mask,Object unused){return a.prime(t,s,p,w,h,g,close,epoch,c,starting,unwarped);}}''',
'ng/lst/genie/SnapshotGuard.java': '''package ng.lst.genie; import io.github.libxposed.api.*;public class SnapshotGuard {public SnapshotGuard(XposedModule m,ClassLoader c){}public boolean block(int t,long e){return true;}}''',
'ng/lst/genie/SurfaceEvents.java': '''package ng.lst.genie; import kotlin.jvm.functions.*; import io.github.libxposed.api.*;public class SurfaceEvents {public SurfaceEvents(XposedModule m,ClassLoader c,Function1 a,Function0 b,Function1 d,Function1 e){}public static class Show {}}''',
'ng/lst/genie/LiveSubtrees.java': '''package ng.lst.genie; import kotlin.jvm.functions.*;import io.github.libxposed.api.*;public class LiveSubtrees {public LiveSubtrees(XposedModule m,ClassLoader c,Function0 f){}public static class Entry implements AutoCloseable {public void close(){}}public Entry ready(int i){return null;}}''',
'PreviewCloseHarness.java': r'''import java.util.*;import java.lang.reflect.*;import android.graphics.*;import android.view.*;import android.os.*;import android.content.*;import ng.lst.genie.*;
public class PreviewCloseHarness {
 static int checks;static final Rect FRAME=new Rect(0,0,1120,2436);
 public static class Display {public Rect getBounds(){return FRAME;}public int getRotation(){return 0;}}
 public static class Task {public int mTaskId=44;public SurfaceControl mSurfaceControl=new SurfaceControl(10);public Object getParent(){return null;}public int getWindowingMode(){return 1;}public Rect getBounds(){return FRAME;}}
 public static class Activity {public Task task;public Object token=new Object(),mStartingData=new Object();public android.content.pm.ActivityInfo info=new android.content.pm.ActivityInfo();public Activity(Task t){task=t;}public Task getTask(){return task;}public Task getParent(){return task;}public int getWindowingMode(){return 1;}public boolean isEmbedded(){return false;}public boolean hasFixedRotationTransform(){return false;}public boolean isLetterboxedForFixedOrientationAndAspectRatio(){return false;}public Rect getBounds(){return FRAME;}public Display getDisplayContent(){return DISPLAY;}}
 static final Display DISPLAY=new Display();
 public static class Animator {public SurfaceControl mSurfaceControl=new SurfaceControl(20);}
 public static class Window {public boolean mRemoved=false,mHasSurface=true;public int mViewVisibility=0,mOwnerUid=10042;public WindowManager.LayoutParams mAttrs=new WindowManager.LayoutParams();public Activity mActivityRecord;public Animator mWinAnimator=new Animator();public SurfaceControl mSurfaceControl=new SurfaceControl(19);public Window(Activity a){mActivityRecord=a;}public Task getTask(){return mActivityRecord.task;}public Rect getFrame(){return FRAME;}public Activity getParent(){return mActivityRecord;}}
 public static class Service {public Object mGlobalLock=new Object();public Map<Object,Object> mWindowMap=new LinkedHashMap<>();}
 static Object field(Object o,String name)throws Exception {Field f=o.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(o);}
 static void set(Object o,String name,Object v)throws Exception {Field f=o.getClass().getDeclaredField(name);f.setAccessible(true);f.set(o,v);}
 static void check(boolean value,String m){checks++;if(!value)throw new AssertionError(m);}
 static Object request(boolean closing,int parent,long time,float width,float height)throws Exception {Class<?> c=Class.forName("ng.lst.genie.CentralBridge$Request");Constructor<?> k=c.getDeclaredConstructors()[0];k.setAccessible(true);return k.newInstance(4,99,parent,44,closing,time,new float[]{0,0,width,height,900,2200,170,170,0,0,180,45});}
 static Object call(Object b,String method,Object...args)throws Exception {Method m=Arrays.stream(b.getClass().getDeclaredMethods()).filter(x->x.getName().equals(method)).findFirst().get();m.setAccessible(true);try{return m.invoke(b,args);}catch(InvocationTargetException e){throw (Exception)e.getCause();}}
 static long epoch=200;static Object bridge;static Service service;static Window window;static Activity activity;static Task task;static Map<Object,Object> tokens;static Map<Object,Object> grants;static Object grant;
 static boolean close(int parent,float width,float height)throws Exception {return (boolean)call(bridge,"prepareGroup",service,new Context(),request(true,parent,++epoch,width,height),Parcel.obtain());}
 public static void main(String[]args)throws Exception {
  bridge=new CentralBridge(new io.github.libxposed.api.XposedModule(),PreviewCloseHarness.class.getClassLoader());task=new Task();activity=new Activity(task);window=new Window(activity);service=new Service();service.mWindowMap.put(1,window);
  Object previews=field(bridge,"preStarting");tokens=(Map<Object,Object>)field(previews,"currentLaunchTokens");tokens.put(activity,77L);
  set(bridge,"rendererReady",true);
  Object opening=request(false,10,100,1120,2436);Object snapshot=call(bridge,"lookup",service,opening,null);grant=field(snapshot,"previewIdentity");check(grant!=null,"opening supplies exact capability");((AutoCloseable)snapshot).close();grants=(Map<Object,Object>)field(bridge,"openingPreviews");grants.put(44,grant);
  var manager=com.android.server.wm.PreStartingManager.INSTANCE;var color=new SurfaceControl(30);color.mWidth=color.mHeight=0;manager.mColorSurfaceControlMap.put(activity.mStartingData,color);
  Parcel reply=Parcel.obtain();check((boolean)call(bridge,"prepareGroup",service,new Context(),request(true,10,++epoch,560,1218),reply),"exact authenticated preview closes without main source");check(reply.values.equals(List.of(1,1120,2436)),"existing group reply carries verified full domain");check(ContentAnimator.surface==20&&ContentAnimator.parent==10&&Arrays.equals(ContentAnimator.companions,new long[]{30}),"preview and exact manager color prime together");check(SurfaceControl.applies==0,"borrowed host publication is never independently applied");
  int before=ContentAnimator.primes;check(!close(999,1120,2436),"unrelated new host/container denied");check(!close(10,560,1100),"nonuniform close denied");check(!close(10,1121,2436*1121f/1120),"domain tolerance remains within existing bound");
 }
}''',
}
# Complete the cases separately to keep the protocol fixture readable.
SOURCES['PreviewCloseHarness.java'] = SOURCES['PreviewCloseHarness.java'].replace('check(!close(10,1121,2436*1121f/1120),"domain tolerance remains within existing bound");', r'''
  window.mRemoved=true;check(!close(10,1120,2436),"removed original window denied");window.mRemoved=false;
  service.mWindowMap.clear();var recreated=new Window(activity);recreated.mWinAnimator.mSurfaceControl.id=20;service.mWindowMap.put(2,recreated);check(!close(10,1120,2436),"recreated window with same layer denied");service.mWindowMap.clear();service.mWindowMap.put(1,window);
  SurfaceControl original=window.mWinAnimator.mSurfaceControl;window.mWinAnimator.mSurfaceControl=new SurfaceControl(20);check(!close(10,1120,2436),"recreated producer at same layer denied");window.mWinAnimator.mSurfaceControl=original;
  ContentSurfaces.replacement=21;check(!close(10,1120,2436),"changed resolved BBQ denied");ContentSurfaces.replacement=-1;
  tokens.put(activity,78L);check(!close(10,1120,2436),"reused Activity with new launch token denied");tokens.put(activity,77L);
  call(previews,"recordCurrentLaunch",activity,null);check(!close(10,1120,2436)&&!tokens.containsKey(activity),"tokenless options invalidate previous generation");call(previews,"recordCurrentLaunch",activity,77L);
  call(previews,"recordCurrentLaunch",activity,0L);check(!close(10,1120,2436)&&!tokens.containsKey(activity),"zero-token options invalidate previous generation");call(previews,"recordCurrentLaunch",activity,77L);
  Object token=activity.token;activity.token=new Object();check(!close(10,1120,2436),"changed Activity token denied");activity.token=token;
  Task originalTask=activity.task;activity.task=new Task();check(!close(10,1120,2436),"recreated Task at same id denied");activity.task=originalTask;
  service.mWindowMap.clear();check(!close(10,1120,2436),"expired WindowState registry membership denied");service.mWindowMap.put(1,window);
  ((Map<Object,Object>)field(bridge,"taskEvents")).put(44,Long.MAX_VALUE);check(!close(10,1120,2436),"superseded close epoch denied");((Map<?,?>)field(bridge,"taskEvents")).clear();
  grants.clear();check(!close(10,1120,2436),"no grant cannot rediscover arbitrary starting window");check(ContentAnimator.primes==before,"all rejected cases preserve publication count");
  System.out.println("PASS "+checks+" authenticated preview-close bridge checks");''')

with tempfile.TemporaryDirectory(prefix='genie-preview-close-jvm-') as temp:
    temp = pathlib.Path(temp)
    for name, source in SOURCES.items():
        target = temp / 'src' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source)
    cp = os.pathsep.join(map(str, [CLASSES, STDLIB, API]))
    java_sources = [str(p) for p in (temp / 'src').rglob('*.java')]
    subprocess.run(['javac', '-Xlint:-unchecked', '-cp', cp, '-d', str(temp / 'classes')] + java_sources, check=True)
    subprocess.run(['java', '-cp', str(temp / 'classes') + os.pathsep + cp, 'PreviewCloseHarness'], check=True)
