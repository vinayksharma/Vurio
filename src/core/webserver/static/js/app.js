// Media Deduplication Server Web Interface
class MediaDedupApp {
    constructor() {
        this.currentSection = 'config';
        this.init();
    }

    init() {
        this.setupEventListeners();
        this.loadConfiguration();
        this.loadTPMStatus();
        this.loadUserSettings();
        this.loadMediaLocations();
    }

    setupEventListeners() {
        // Navigation
        document.querySelectorAll('.nav-link').forEach(link => {
            link.addEventListener('click', (e) => {
                e.preventDefault();
                const section = e.target.getAttribute('href').substring(1);
                this.showSection(section);
            });
        });

        // Configuration controls
        document.getElementById('reload-config').addEventListener('click', () => {
            this.reloadConfiguration();
        });

        document.getElementById('save-config').addEventListener('click', () => {
            this.saveConfiguration();
        });

        // User settings controls
        document.getElementById('add-setting').addEventListener('click', () => {
            this.addUserSetting();
        });

        // Media location controls
        document.getElementById('register-media').addEventListener('click', () => {
            this.registerMediaLocation();
        });

        document.getElementById('deregister-media').addEventListener('click', () => {
            this.deregisterMediaLocation();
        });
    }

    showSection(sectionName) {
        // Hide all sections
        document.querySelectorAll('.section').forEach(section => {
            section.classList.remove('active');
        });

        // Remove active class from all nav links
        document.querySelectorAll('.nav-link').forEach(link => {
            link.classList.remove('active');
        });

        // Show selected section
        document.getElementById(sectionName).classList.add('active');
        
        // Add active class to corresponding nav link
        document.querySelector(`[href="#${sectionName}"]`).classList.add('active');
        
        this.currentSection = sectionName;
    }

    async apiCall(endpoint, method = 'GET', data = null) {
        try {
            const options = {
                method,
                headers: {
                    'Content-Type': 'application/json',
                }
            };

            if (data) {
                options.body = JSON.stringify(data);
            }

            const response = await fetch(endpoint, options);
            
            if (!response.ok) {
                throw new Error(`HTTP error! status: ${response.status}`);
            }

            return await response.json();
        } catch (error) {
            console.error('API call failed:', error);
            this.showError(`API call failed: ${error.message}`);
            return null;
        }
    }

    async loadConfiguration() {
        const config = await this.apiCall('/api/v1/config');
        if (config) {
            this.displayConfiguration(config);
        }
    }

    displayConfiguration(config) {
        const grid = document.getElementById('config-grid');
        grid.innerHTML = '';

        Object.entries(config).forEach(([key, value]) => {
            const item = document.createElement('div');
            item.className = 'config-item';
            
            const valueStr = typeof value === 'object' ? JSON.stringify(value, null, 2) : String(value);
            
            item.innerHTML = `
                <h3>${key}</h3>
                <p>Current value:</p>
                <div class="value-display">${valueStr}</div>
                <input type="text" id="edit-${key}" placeholder="New value" value="${valueStr}">
                <button onclick="app.updateConfigProperty('${key}')" class="btn btn-primary">Update</button>
            `;
            
            grid.appendChild(item);
        });
    }

    async updateConfigProperty(key) {
        const input = document.getElementById(`edit-${key}`);
        const newValue = input.value;

        const result = await this.apiCall(`/api/v1/config/${key}`, 'PUT', { value: newValue });
        if (result) {
            this.showSuccess(`Property ${key} updated successfully`);
            this.loadConfiguration(); // Reload to show updated values
        }
    }

    async reloadConfiguration() {
        const result = await this.apiCall('/api/v1/config/reload', 'POST');
        if (result) {
            this.showSuccess('Configuration reloaded successfully');
            this.loadConfiguration();
        }
    }

    async saveConfiguration() {
        // This would need to be implemented as an API endpoint
        this.showInfo('Save configuration feature coming soon');
    }

    async loadTPMStatus() {
        const status = await this.apiCall('/api/v1/tpm/status');
        if (status) {
            this.displayTPMStatus(status);
        }
    }

    displayTPMStatus(status) {
        const container = document.getElementById('tpm-status');
        container.innerHTML = '';

        Object.entries(status).forEach(([key, value]) => {
            const item = document.createElement('div');
            item.className = 'status-item';
            item.innerHTML = `
                <h4>${key}</h4>
                <p>${JSON.stringify(value, null, 2)}</p>
            `;
            container.appendChild(item);
        });
    }

    async loadUserSettings() {
        const settings = await this.apiCall('/api/v1/user-settings');
        if (settings) {
            this.displayUserSettings(settings);
        }
    }

    displayUserSettings(settings) {
        const container = document.getElementById('settings-list');
        container.innerHTML = '';

        if (settings.length === 0) {
            container.innerHTML = '<p>No user settings found.</p>';
            return;
        }

        settings.forEach(setting => {
            const item = document.createElement('div');
            item.className = 'status-item';
            item.innerHTML = `
                <h4>${setting.key}</h4>
                <p>${setting.value}</p>
                <button onclick="app.deleteUserSetting('${setting.key}')" class="btn btn-danger">Delete</button>
            `;
            container.appendChild(item);
        });
    }

    async addUserSetting() {
        const key = document.getElementById('new-setting-key').value;
        const value = document.getElementById('new-setting-value').value;

        if (!key || !value) {
            this.showError('Please enter both key and value');
            return;
        }

        const result = await this.apiCall(`/api/v1/user-settings/${key}`, 'PUT', { value });
        if (result) {
            this.showSuccess('User setting added successfully');
            document.getElementById('new-setting-key').value = '';
            document.getElementById('new-setting-value').value = '';
            this.loadUserSettings();
        }
    }

    async deleteUserSetting(key) {
        if (confirm(`Are you sure you want to delete setting "${key}"?`)) {
            const result = await this.apiCall(`/api/v1/user-settings/${key}`, 'DELETE');
            if (result) {
                this.showSuccess('User setting deleted successfully');
                this.loadUserSettings();
            }
        }
    }

    async loadMediaLocations() {
        // This would need to be implemented as an API endpoint
        const container = document.getElementById('media-list');
        container.innerHTML = '<p>Media locations feature coming soon</p>';
    }

    async registerMediaLocation() {
        const path = document.getElementById('media-path').value;
        if (!path) {
            this.showError('Please enter a media path');
            return;
        }

        const result = await this.apiCall('/api/v1/media-locations/register', 'POST', { directory: path });
        if (result) {
            this.showSuccess('Media location registered successfully');
            document.getElementById('media-path').value = '';
            this.loadMediaLocations();
        }
    }

    async deregisterMediaLocation() {
        const path = document.getElementById('media-path').value;
        if (!path) {
            this.showError('Please enter a media path');
            return;
        }

        const result = await this.apiCall('/api/v1/media-locations/deregister', 'POST', { directory: path });
        if (result) {
            this.showSuccess('Media location deregistered successfully');
            document.getElementById('media-path').value = '';
            this.loadMediaLocations();
        }
    }

    showSuccess(message) {
        this.showNotification(message, 'success');
    }

    showError(message) {
        this.showNotification(message, 'error');
    }

    showInfo(message) {
        this.showNotification(message, 'info');
    }

    showNotification(message, type) {
        // Simple notification system
        const notification = document.createElement('div');
        notification.style.cssText = `
            position: fixed;
            top: 20px;
            right: 20px;
            padding: 15px 20px;
            border-radius: 4px;
            color: white;
            font-weight: bold;
            z-index: 1000;
            max-width: 300px;
        `;

        switch (type) {
            case 'success':
                notification.style.backgroundColor = '#27ae60';
                break;
            case 'error':
                notification.style.backgroundColor = '#e74c3c';
                break;
            case 'info':
                notification.style.backgroundColor = '#3498db';
                break;
        }

        notification.textContent = message;
        document.body.appendChild(notification);

        setTimeout(() => {
            document.body.removeChild(notification);
        }, 3000);
    }
}

// Initialize the app when the page loads
document.addEventListener('DOMContentLoaded', () => {
    window.app = new MediaDedupApp();
});
