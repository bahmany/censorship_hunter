package com.hunter.app;

import android.content.Context;
import android.opengl.GLSurfaceView;
import android.view.MotionEvent;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class HunterView extends GLSurfaceView {

    private final Renderer renderer;

    public HunterView(Context context) {
        super(context);
        setEGLContextClientVersion(3);
        setPreserveEGLContextOnPause(true);
        renderer = new Renderer();
        setRenderer(renderer);
        setRenderMode(RENDERMODE_CONTINUOUSLY);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        float x = event.getX() / getResources().getDisplayMetrics().density;
        float y = event.getY() / getResources().getDisplayMetrics().density;
        int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                ((MainActivity) getContext()).sendTouchEvent(action, x, y);
                break;
        }
        return true;
    }

    private static class Renderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            nativeOnSurfaceCreated();
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            nativeOnSurfaceChanged(width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            nativeOnDrawFrame();
        }

        private static native void nativeOnSurfaceCreated();
        private static native void nativeOnSurfaceChanged(int width, int height);
        private static native void nativeOnDrawFrame();
    }
}
