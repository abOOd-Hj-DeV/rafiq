package dev.k230.red_demo;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private LinearLayout page;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        neutral();
    }

    private TextView text(String content, int size) {
        TextView label = new TextView(this);
        label.setText(content);
        label.setTextSize(size);
        label.setTextColor(Color.rgb(24, 81, 77));
        label.setGravity(Gravity.CENTER);
        label.setPadding(24, 24, 24, 24);
        return label;
    }

    private void neutral() {
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER);
        page.setBackgroundColor(Color.rgb(242, 249, 246));
        page.addView(text("K230\nاختبار الكشف والاستجابة", 30));
        page.addView(text("عرض تجريبي، وليس تصنيف محتوى ضار.\nانتظر جاهزية اللوحة، ثم اعرض الشاشة الحمراء.", 19));
        Button red = new Button(this);
        red.setText("عرض شاشة الاختبار الحمراء");
        red.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { solid(Color.RED); }
        });
        page.addView(red);
        Button blue = new Button(this);
        blue.setText("اختبار اللون الأزرق — لا يجب حجبه");
        blue.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { solid(Color.BLUE); }
        });
        page.addView(blue);
        setContentView(page);
    }

    private void solid(int color) {
        View screen = new View(this);
        screen.setBackgroundColor(color);
        screen.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { neutral(); }
        });
        setContentView(screen);
    }
}
