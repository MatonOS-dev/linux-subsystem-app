package org.matonos.linux.store;

import org.json.*;
import org.matonos.store.source.types.*;
import java.util.*;

/** The binder boundary: maps internal AppStream rows to the shared typed contract. */
public final class SourceTypes {
    public static AppListing listing(JSONObject row) {
        AppListing out=new AppListing();out.id=row.optString("id");out.repo=row.optString("origin");
        out.name=row.optString("name");out.summary=row.optString("summary");out.icon=row.optString("icon");
        out.developer=row.optString("developer");out.installed=row.optBoolean("installed");
        out.added=epochSeconds(row.opt("added"));
        JSONArray options=row.optJSONArray("repoOptions");if(options!=null){out.repoOptions=new RepoOption[options.length()];for(int i=0;i<out.repoOptions.length;i++){JSONObject value=options.optJSONObject(i);if(value==null)continue;RepoOption option=new RepoOption();option.repoId=value.optString("repoId");option.name=value.optString("name");option.version=value.optString("version",null);option.installable=value.optBoolean("installable",true);option.note=value.optString("note",null);out.repoOptions[i]=option;}}
        JSONArray badgeRows=row.optJSONArray("badges");if(badgeRows!=null){out.badges=new AppBadge[badgeRows.length()];for(int i=0;i<out.badges.length;i++){JSONObject value=badgeRows.optJSONObject(i);if(value==null)continue;AppBadge badge=new AppBadge();badge.id=value.optString("id");badge.label=value.optString("label");badge.icon=value.optString("icon");out.badges[i]=badge;}}
        out.versionName=row.optString("versionName",null);out.packageName=row.optString("packageName",null);return out;
    }
    public static AppListing[] listings(List<Map<String,Object>> rows,int offset,int limit){
        List<Map<String,Object>> page=SourcePaging.page(rows,offset,limit);AppListing[] result=new AppListing[page.size()];
        for(int i=0;i<result.length;i++)result[i]=listing(new JSONObject(page.get(i)));return result;
    }
    public static AppListing[] listings(JSONArray rows,int offset,int limit)throws JSONException {
        int end=SourcePaging.end(rows.length(),offset,limit);AppListing[] result=new AppListing[Math.max(0,end-offset)];
        for(int i=0;i<result.length;i++)result[i]=listing(rows.getJSONObject(offset+i));return result;
    }
    public static String[] strings(JSONArray values)throws JSONException {
        if(values==null)return new String[0];String[] out=new String[values.length()];for(int i=0;i<out.length;i++)out[i]=values.getString(i);return out;
    }
    public static long epochSeconds(Object... values) {
        for(Object value:values) {
            if(value instanceof Number)return ((Number)value).longValue();
            if(!(value instanceof String)||((String)value).isBlank())continue;
            String text=((String)value).trim();
            try{return Long.parseLong(text);}catch(NumberFormatException ignored){}
            try{return java.time.Instant.parse(text).getEpochSecond();}catch(java.time.format.DateTimeParseException ignored){}
            try{return java.time.OffsetDateTime.parse(text).toEpochSecond();}catch(java.time.format.DateTimeParseException ignored){}
            try{return java.time.LocalDate.parse(text).atStartOfDay(java.time.ZoneOffset.UTC).toEpochSecond();}catch(java.time.format.DateTimeParseException ignored){}
        }
        return 0;
    }
    public static AppDetails details(JSONObject row)throws JSONException {
        AppDetails out=new AppDetails();out.listing=listing(row);out.description=row.optString("description");out.descriptionMarkdown=row.optString("descriptionMarkdown",null);
        out.screenshots=strings(row.optJSONArray("screenshots"));out.permissions=strings(row.optJSONArray("permissions"));
        out.license=row.optString("license",null);out.homepage=row.optString("homepage",null);
        out.downloadSize=row.optLong("downloadSize",-1);out.installedSize=row.optLong("installedSize",-1);
        JSONObject age=row.optJSONObject("ageRating");if(age!=null){
            out.ageRating=new AgeRating();out.ageRating.minimumAge=age.getInt("minimumAge");out.ageRating.system=age.getString("system");out.ageRating.label=age.getString("label");
            JSONArray categories=age.getJSONArray("categories");out.ageRating.categories=new AgeRatingCategory[categories.length()];
            for(int i=0;i<categories.length();i++){JSONObject value=categories.getJSONObject(i);AgeRatingCategory item=new AgeRatingCategory();item.id=value.getString("id");item.title=value.getString("title");item.description=value.getString("description");item.level=value.getString("level");out.ageRating.categories[i]=item;}
        }return out;
    }

    public static Review[] reviews(JSONArray rows,int offset,int limit)throws JSONException {
        int end=SourcePaging.end(rows.length(),offset,limit);Review[] result=new Review[Math.max(0,end-offset)];
        for(int i=0;i<result.length;i++){JSONObject row=rows.getJSONObject(offset+i);Review out=new Review();out.id=row.getString("id");out.author=row.getString("author");out.rating=Math.max(1,Math.min(5,(int)Math.round(row.getDouble("rating"))));out.summary=row.getString("summary");out.description=row.getString("description");out.version=row.getString("version");out.date=java.time.Instant.parse(row.getString("date")).toEpochMilli();result[i]=out;}return result;
    }
    private SourceTypes(){}
}
