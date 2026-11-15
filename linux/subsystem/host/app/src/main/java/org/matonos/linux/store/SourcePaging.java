package org.matonos.linux.store;
import java.util.*;
public final class SourcePaging {
    public static int end(int size,int offset,int limit){
        if(offset<0||limit<=0)throw new IllegalArgumentException("offset must be nonnegative and limit positive");
        return (int)Math.min(size,(long)offset+limit);
    }
    public static <T> List<T> page(List<T> rows,int offset,int limit){int end=end(rows.size(),offset,limit);return offset>=end?Collections.emptyList():rows.subList(offset,end);}
    private SourcePaging(){}
}
