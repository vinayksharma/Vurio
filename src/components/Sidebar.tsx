import type { PanelType } from '../types';

interface SidebarProps {
  activePanel: PanelType;
  onPanelChange: (panel: PanelType) => void;
}

const Sidebar: React.FC<SidebarProps> = ({ activePanel, onPanelChange }) => {
  const navItems: Array<{ id: PanelType; icon: string; label: string }> = [
    { id: 'media', icon: '📁', label: 'Media Locations' },
    { id: 'duplicates', icon: '🫂', label: 'Duplicates' },
    { id: 'settings', icon: '⚙️', label: 'Settings' },
    { id: 'advanced', icon: '🧰', label: 'Advanced' },
  ];

  return (
    <aside className="sidebar">
      <div className="sidebar-header">
        <div className="sidebar-title">VURIO</div>
        <div className="sidebar-subtitle">Media Management</div>
      </div>
      <nav className="sidebar-nav">
        {navItems.map((item) => (
          <button
            key={item.id}
            className={`nav-item ${activePanel === item.id ? 'active' : ''}`}
            onClick={() => onPanelChange(item.id)}
          >
            <span className="nav-icon" aria-hidden="true">
              {item.icon}
            </span>
            <span className="nav-label">{item.label}</span>
          </button>
        ))}
      </nav>
    </aside>
  );
};

export default Sidebar;
