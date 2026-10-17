package org.matonos.linux.stub;

public final class InstallProgressTest {
    public static void main(String[] args) {
        String[] output={"Installing…  0%","Receiving objects…  42%  512 KiB / 1.2 MB","Installing… 100%"};
        int[] expected={0,42,100};
        for(int i=0;i<output.length;i++)if(InstallProgress.parsePercent(output[i])!=expected[i])throw new AssertionError(output[i]);
        if(InstallProgress.parsePercent("No progress")!=-1)throw new AssertionError("unknown progress must stay unknown");
        if(InstallProgress.weightedPercent(40,60,50,100)!=70)throw new AssertionError("weighted percentage");
        if(InstallProgress.weightedPercent(90,10,30,100)<InstallProgress.weightedPercent(40,60,50,100))throw new AssertionError("monotonic sequence");
        InstallProgress monotonic=new InstallProgress();
        if(monotonic.combine(70)!=70||monotonic.combine(20)!=70)throw new AssertionError("monotonic clamp");
        System.out.println("Install progress parser and weighting tests passed");
    }
}
