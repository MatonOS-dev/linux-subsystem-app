package org.matonos.linux;

import org.junit.Test;
import static org.junit.Assert.*;
import org.json.*;
import org.matonos.linux.store.*;
import org.matonos.store.source.types.*;

public class SourceTypesTest {
    private JSONArray listings() throws Exception {
        JSONArray rows=new JSONArray();
        for(int i=0;i<5;i++)rows.put(new JSONObject().put("id","app/example/x86_64/stable").put("origin","remote-"+i).put("name","Example").put("packageName","flatpak.example"));
        return rows;
    }
    @Test public void listingPagesKeepRemote() throws Exception {
        AppListing[] page=SourceTypes.listings(listings(),2,2);
        assertEquals(2,page.length);assertEquals("remote-2",page[0].repo);assertEquals("app/example/x86_64/stable",page[0].id);assertEquals("flatpak.example",page[0].packageName);
        assertEquals(1,SourceTypes.listings(listings(),4,2).length);assertEquals(0,SourceTypes.listings(listings(),9,2).length);
    }
    @Test public void everyListIsPaged() throws Exception {
        JSONArray repos=new JSONArray(),reviews=new JSONArray();
        for(int i=0;i<3;i++){
            repos.put(new JSONObject().put("id","remote-"+i).put("url","https://example.org/").put("label","Example").put("official",false).put("broken",i==2));
            reviews.put(new JSONObject().put("id","review-"+i).put("author","Example").put("rating",4.5).put("summary","").put("description","").put("version","1").put("date","2026-01-01T00:00:00Z"));
        }
        assertEquals("remote-2",SourceTypes.repos(repos,2,2)[0].id);assertTrue(SourceTypes.repos(repos,2,2)[0].broken);
        assertEquals(0,SourceTypes.repos(repos,3,2).length);
        Review[] page=SourceTypes.reviews(reviews,2,2);assertEquals(1,page.length);assertEquals("review-2",page[0].id);assertEquals(1767225600000L,page[0].date);assertEquals(5,page[0].rating);
        assertEquals(0,SourceTypes.reviews(reviews,3,2).length);
    }
    @Test public void detailsPreserveTypedMetadata() throws Exception {
        JSONObject row=listings().getJSONObject(0).put("description","Description").put("downloadSize",123).put("installedSize",456)
                .put("screenshots",new JSONArray().put("https://example.org/screen.png")).put("permissions",new JSONArray().put("network"))
                .put("ageRating",new JSONObject().put("minimumAge",12).put("system","PEGI").put("label","12").put("categories",new JSONArray().put(new JSONObject().put("id","violence").put("title","Violence").put("description","").put("level","mild"))));
        AppDetails out=SourceTypes.details(row);assertEquals("remote-0",out.listing.repo);assertEquals(123,out.downloadSize);assertEquals(456,out.installedSize);assertEquals("network",out.permissions[0]);assertEquals(12,out.ageRating.minimumAge);assertEquals("mild",out.ageRating.categories[0].level);
    }
}
