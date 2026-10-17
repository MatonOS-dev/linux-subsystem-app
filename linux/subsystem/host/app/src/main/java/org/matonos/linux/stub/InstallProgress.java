package org.matonos.linux.stub;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Parses the pinned Flatpak CLI's stable non-TTY percentage line and combines phases. */
final class InstallProgress {
    private static final Pattern PERCENT = Pattern.compile("(?:^|\\s)(\\d{1,3})%(?:\\s|$)");
    private int last = -1;

    static int parsePercent(String line) {
        Matcher matcher = PERCENT.matcher(line == null ? "" : line);
        int value = -1;
        while (matcher.find()) value = Math.min(100, Integer.parseInt(matcher.group(1)));
        return value;
    }

    synchronized int combine(int phasePercent) {
        if (phasePercent < 0) return last;
        last = Math.max(last, Math.min(100, phasePercent));
        return last;
    }

    static int weightedPercent(long completedWeight,long currentWeight,int currentPercent,long totalWeight) {
        if(totalWeight<=0||currentPercent<0)return -1;
        long done=Math.max(0,completedWeight)+Math.max(0,currentWeight)*Math.min(100,currentPercent)/100;
        return (int)Math.min(100,done*100/totalWeight);
    }
}
