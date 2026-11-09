package org.matonos.tray;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.Collections;
import org.junit.Test;

public final class MenuViewModelTest {
  @Test
  public void keepsSubmenusAndToggleState() {
    TrayItem.MenuEntry child =
        entry(3, "Enabled", true, true, false, "checkmark", 1, Collections.emptyList());
    TrayItem.MenuEntry parent =
        entry(2, "Options", true, true, false, "", 0, Collections.singletonList(child));

    MenuViewModel.Node node = MenuViewModel.from(Collections.singletonList(parent)).children.get(0);

    assertEquals("Options", node.label);
    assertEquals(1, node.children.size());
    assertEquals("checkmark", node.children.get(0).toggleType);
    assertEquals(1, node.children.get(0).toggleState);
  }

  @Test
  public void removesHiddenNodesAtEveryDepth() {
    TrayItem.MenuEntry hiddenChild =
        entry(4, "Hidden child", true, false, false, "", 0, Collections.emptyList());
    TrayItem.MenuEntry visibleParent =
        entry(2, "Parent", true, true, false, "", 0, Arrays.asList(hiddenChild));
    TrayItem.MenuEntry hiddenParent =
        entry(5, "Hidden parent", true, false, false, "", 0, Collections.emptyList());

    MenuViewModel model = MenuViewModel.from(Arrays.asList(visibleParent, hiddenParent));

    assertEquals(1, model.children.size());
    assertTrue(model.children.get(0).children.isEmpty());
    assertFalse(model.children.stream().anyMatch(node -> node.id == 5));
  }

  private TrayItem.MenuEntry entry(
      int id,
      String label,
      boolean enabled,
      boolean visible,
      boolean separator,
      String toggleType,
      int toggleState,
      java.util.List<TrayItem.MenuEntry> children) {
    return new TrayItem.MenuEntry(
        id, label, enabled, visible, separator, toggleType, toggleState, null, children);
  }
}
